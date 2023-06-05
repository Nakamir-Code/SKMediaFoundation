#include "mf_decode_to_buffer.h"
#include "error.h"
#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <codecapi.h>
#include <vector>
#include <d3d11.h>
#include <thread>

EXTERN_GUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, 0xc60ac5fe, 0x252a, 0x478f, 0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3);
EXTERN_GUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID, 0x8ac3587a, 0x4ae7, 0x42d8, 0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f);

EXTERN_GUID(D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 0x1b81be68, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace nakamir {

	// We need these fields to stay alive when out of scope
	std::thread sourceReaderThread;
	ComPtr<IMFSourceReader> pSourceReader;
	ComPtr<IMFMediaType> pFileVideoMediaType;
	ComPtr<IMFTransform> pDecoderTransform; // This is the H264 Decoder MFT
	IMFActivate** ppActivate = NULL;
	nv12_tex_t _nv12_tex;

	// GPU-specific requirements
	HANDLE deviceHandle;
	ComPtr<IMFDXGIDeviceManager> pDXGIDeviceManager;
	ComPtr<ID3D11VideoContext> pVideoContext;
	ComPtr<ID3D11VideoDecoder> pVideoDecoder;
	ComPtr<ID3D11Texture2D> pOutputTexture;
	std::vector<ComPtr<IMFSample>> outputSamples;
	std::vector<ComPtr<ID3D11VideoDecoderOutputView>> outputViews;

	void print_stats();
	void create_dx_video_decoder();
	void create_fallback_video_decoder();
	void decode_loop_cpu();
	void decode_loop_gpu();
	void shutdown();
	void release_dx_resources();

	void mf_mp4_source_reader(const wchar_t* filename, nv12_tex_t nv12_tex)
	{
		_nv12_tex = nv12_tex;
		try
		{
			// Start up the Media Foundation platform
			ThrowIfFailed(MFStartup(MF_VERSION));

			// Set up the reader for the file
			ComPtr<IMFSourceResolver> pSourceResolver;
			ThrowIfFailed(MFCreateSourceResolver(&pSourceResolver));

			// Create a media source object from a URL
			MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;
			ComPtr<IUnknown> uSource;
			ThrowIfFailed(pSourceResolver->CreateObjectFromURL(
				filename,					// URL of the source.
				MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ,  // Create a source object.
				NULL,                       // Optional property store.
				&ObjectType,				// Receives the created object type. 
				&uSource					// Receives a pointer to the media source.
			));

			// Get a pointer to the media source
			ComPtr<IMFMediaSource> mediaFileSource;
			ThrowIfFailed(uSource->QueryInterface(IID_PPV_ARGS(&mediaFileSource)));

			// Create attributes for the source reader
			ComPtr<IMFAttributes> pVideoReaderAttributes;
			ThrowIfFailed(MFCreateAttributes(&pVideoReaderAttributes, 0));

			// Create a source reader from the media source
			ThrowIfFailed(MFCreateSourceReaderFromMediaSource(mediaFileSource.Get(), pVideoReaderAttributes.Get(), &pSourceReader));

			// Get the current media type of the first video stream
			ThrowIfFailed(pSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pFileVideoMediaType));

			// Get the major and subtype of the mp4 video
			GUID majorType = {};
			GUID subType = {};
			ThrowIfFailed(pFileVideoMediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType));
			ThrowIfFailed(pFileVideoMediaType->GetGUID(MF_MT_SUBTYPE, &subType));

			// Create H.264 decoder.
			MFT_REGISTER_TYPE_INFO inputType = {};
			inputType.guidMajorType = majorType;
			inputType.guidSubtype = subType;

			// Enumerate decoders that match the input type
			UINT32 count = 0;
			ThrowIfFailed(MFTEnumEx(
				MFT_CATEGORY_VIDEO_DECODER,
				MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ALL,
				&inputType,
				NULL,
				&ppActivate,
				&count
			));

			// Check if any decoders were found
			if (count <= 0)
			{
				throw std::exception("No decoders found! :(");
			}

			for (UINT32 i = 0; i < count; i++)
			{
				LPWSTR pszName = nullptr;
				UINT32 pszLength;
				if (FAILED(ppActivate[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &pszName, &pszLength)))
				{
					continue;
				}

				OutputDebugStringW(pszName);
				CoTaskMemFree(pszName);
			}

			// Activate first decoder object and get a pointer to it
			ThrowIfFailed(ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pDecoderTransform)));

			ComPtr<IMFAttributes> pAttributes;
			ThrowIfFailed(pDecoderTransform->GetAttributes(&pAttributes));

			// Tell the decoder to allocate resources for the maximum resolution in
			// order to minimize glitching on resolution changes.
			ThrowIfFailed(pAttributes->SetUINT32(MF_MT_DECODER_USE_MAX_RESOLUTION, 1));
			ThrowIfFailed(pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

			try
			{
				//throw std::exception("TODO: finish decoding. Falling back to software decoding...");

				/******************************************************************
				 * Open a Device Handle
				 *
				 * https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation#open-a-device-handle
				 ******************************************************************/
				ComPtr<IMFAttributes> pAttributes;
				ThrowIfFailed(pDecoderTransform->GetAttributes(&pAttributes));

				// The decoder MFT must expose the MF_SA_D3D_AWARE attribute to TRUE
				UINT32 isD3DAware = false;
				ThrowIfFailed(pAttributes->GetUINT32(MF_SA_D3D_AWARE, &isD3DAware));
				if (isD3DAware)
				{
					ThrowIfFailed(pAttributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE));
				}
				else
				{
					log_warn("Video decoder is not D3D aware, decoding may be slow.");
				}

				// Create the DXGI Device Manager
				UINT resetToken;
				ThrowIfFailed(MFCreateDXGIDeviceManager(&resetToken, &pDXGIDeviceManager));

				// Set the Direct3D 11 device on the DXGI Device Manager
				ID3D11Device* pD3DDevice = (ID3D11Device*)backend_d3d11_get_d3d_device();
				ThrowIfFailed(pDXGIDeviceManager->ResetDevice(pD3DDevice, resetToken));

				// The Topology Loader calls IMFTransform::ProcessMessage with the MFT_MESSAGE_SET_D3D_MANAGER message
				ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(pDXGIDeviceManager.Get())));

				create_dx_video_decoder();
				print_stats();
				sourceReaderThread = std::thread(decode_loop_gpu);
			}
			catch (const std::exception&)
			{
				create_fallback_video_decoder();
				print_stats();
				sourceReaderThread = std::thread(decode_loop_cpu);
			}
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	void create_fallback_video_decoder()
	{
		try
		{
			/******************************************************************
			 * Fallback to Software Decoding
			 *
			 * https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation#fallback-to-software-decoding
			 ******************************************************************/
			ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, NULL));

			// Create output media type for decoder and copy all items from file video media type
			ComPtr<IMFMediaType> pOutputMediaType;
			ThrowIfFailed(MFCreateMediaType(&pOutputMediaType));
			ThrowIfFailed(pFileVideoMediaType->CopyAllItems(pOutputMediaType.Get()));
			ThrowIfFailed(pOutputMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));

			// Set output type for decoder
			ThrowIfFailed(pDecoderTransform->SetOutputType(0, pOutputMediaType.Get(), 0));
			/******************************************************************/
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	void create_dx_video_decoder()
	{
		try
		{
			/******************************************************************
			 * Open a Device Handle (continued)
			 *
			 * https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation#open-a-device-handle
			 ******************************************************************/
			// Call IMFDXGIDeviceManager::OpenDeviceHandle to get a handle to the Direct3D 11 device
			ThrowIfFailed(pDXGIDeviceManager->OpenDeviceHandle(&deviceHandle));

			// Call IMFDXGIDeviceManager::GetVideoService to get a pointer to the D3D11Device
			ID3D11Device* pD3DDevice = (ID3D11Device*)backend_d3d11_get_d3d_device();
			ThrowIfFailed(pDXGIDeviceManager->GetVideoService(deviceHandle, IID_PPV_ARGS(&pD3DDevice)));

			// Call IMFDXGIDeviceManager::GetVideoService to get a pointer to the video accelerator
			ComPtr<ID3D11VideoDevice> pD3DVideoDevice;
			ThrowIfFailed(pDXGIDeviceManager->GetVideoService(deviceHandle, IID_PPV_ARGS(&pD3DVideoDevice)));

			// Call ID3D11Device::GetImmediateContext to get an ID3D11DeviceContext pointer
			ComPtr<ID3D11DeviceContext> pDeviceContext;
			pD3DDevice->GetImmediateContext(&pDeviceContext);

			// Call QueryInterface on the ID3D11DeviceContext to get an ID3D11VideoContext pointer
			ThrowIfFailed(pDeviceContext->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&pVideoContext));

			// It is recommended that you use multi-thread protection on the device context to prevent deadlock issues 
			// that can sometimes happen when you call ID3D11VideoContext::GetDecoderBuffer or ID3D11VideoContext::ReleaseDecoderBuffer
			ComPtr<ID3D10Multithread> pMultiThread;
			ThrowIfFailed(pD3DDevice->QueryInterface(__uuidof(ID3D10Multithread), (void**)&pMultiThread));
			pMultiThread->SetMultithreadProtected(TRUE);
			/******************************************************************/

			/******************************************************************
			 * Find a Decoder Configuration
			 *
			 * https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation#find-a-decoder-configuration
			 ******************************************************************/
			 // Create input media type for decoder and copy all items from file video media type
			ComPtr<IMFMediaType> pInputMediaType;
			ThrowIfFailed(MFCreateMediaType(&pInputMediaType));
			ThrowIfFailed(pFileVideoMediaType->CopyAllItems(pInputMediaType.Get()));
			ThrowIfFailed(pDecoderTransform->SetInputType(0, pInputMediaType.Get(), 0));

			UINT numSupportedProfiles = pD3DVideoDevice->GetVideoDecoderProfileCount();
			GUID desiredDecoderProfile = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
			BOOL isDecoderProfileSupported = FALSE;
			for (UINT i = 0; i < numSupportedProfiles; ++i)
			{
				GUID decoderProfile;
				ThrowIfFailed(pD3DVideoDevice->GetVideoDecoderProfile(i, &decoderProfile));

				if (decoderProfile == desiredDecoderProfile)
				{
					isDecoderProfileSupported = true;
					break;
				}
			}

			if (!isDecoderProfileSupported)
			{
				throw std::exception("No video decoder profiles were found.");
			}

			ThrowIfFailed(pD3DVideoDevice->CheckVideoDecoderFormat(&desiredDecoderProfile, DXGI_FORMAT_NV12, &isDecoderProfileSupported));
			if (!isDecoderProfileSupported)
			{
				throw std::exception("NV12 format is not supported.");
			}

			D3D11_VIDEO_DECODER_DESC videoDecoderDesc = {};
			videoDecoderDesc.Guid = desiredDecoderProfile;
			videoDecoderDesc.OutputFormat = DXGI_FORMAT_NV12;
			videoDecoderDesc.SampleWidth = _nv12_tex->width;
			videoDecoderDesc.SampleHeight = _nv12_tex->height;
			UINT numSupportedConfigs = 0;
			ThrowIfFailed(pD3DVideoDevice->GetVideoDecoderConfigCount(&videoDecoderDesc, &numSupportedConfigs));
			if (numSupportedConfigs == 0)
			{
				throw std::exception("No video decoder configurations were found.");
			}

			std::vector<D3D11_VIDEO_DECODER_CONFIG> videoDecoderConfigList;
			for (UINT i = 0; i < numSupportedConfigs; ++i)
			{
				D3D11_VIDEO_DECODER_CONFIG videoDecoderConfig;
				if (SUCCEEDED(pD3DVideoDevice->GetVideoDecoderConfig(&videoDecoderDesc, i, &videoDecoderConfig)))
				{
					videoDecoderConfigList.push_back(videoDecoderConfig);
				}
			}

			ComPtr<IMFMediaType> pOutputMediaType;
			BOOL isOutputTypeConfigured = FALSE;
			for (int i = 0; SUCCEEDED(pDecoderTransform->GetOutputAvailableType(0, i, &pOutputMediaType)); ++i)
			{
				GUID outSubtype = {};
				ThrowIfFailed(pOutputMediaType->GetGUID(MF_MT_SUBTYPE, &outSubtype));

				if (outSubtype == MFVideoFormat_NV12)
				{
					ThrowIfFailed(pDecoderTransform->SetOutputType(0, pOutputMediaType.Get(), 0));
					isOutputTypeConfigured = TRUE;
					break;
				}
			}

			if (!isOutputTypeConfigured)
			{
				throw std::exception("Failed to set output type.");
			}
			/******************************************************************/

			/******************************************************************
			 * Allocating Uncompressed Buffers
			 *
			 * https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation#find-a-decoder-configuration
			 ******************************************************************/
			ComPtr<IMFAttributes> pOutputStreamAttributes;
			ThrowIfFailed(pDecoderTransform->GetOutputStreamAttributes(0, &pOutputStreamAttributes));

			UINT32 minSampleCount = 3;
			ThrowIfFailed(pOutputStreamAttributes->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, minSampleCount));

			UINT numSurfaces = 0;
			// More reference frames will require slightly more CPU power for playback (TODO: confirm this claim)
			// It's recommended to have at least 3. The default is limited to 1 and max is 16.
			numSurfaces += 3; // Surfaces for reference frames
			numSurfaces += 3; // Surfaces for deinterlacing (three surfaces)
			numSurfaces += 0; // Surfaces that the decoder needs for buffering

			UINT bindFlags = D3D11_BIND_DECODER;
			UINT32 outputStreamBindFlags;
			if (SUCCEEDED(pOutputStreamAttributes->GetUINT32(MF_SA_D3D11_BINDFLAGS, &outputStreamBindFlags)))
			{
				bindFlags |= outputStreamBindFlags;
			}

			// Create a D3D11 texture as the output buffer
			D3D11_TEXTURE2D_DESC textureDesc = {};
			textureDesc.Width = _nv12_tex->width;
			textureDesc.Height = _nv12_tex->height;
			textureDesc.MipLevels = 1;
			textureDesc.ArraySize = numSurfaces;
			textureDesc.Format = DXGI_FORMAT_NV12;
			textureDesc.SampleDesc.Count = 1;
			textureDesc.Usage = D3D11_USAGE_DEFAULT;
			textureDesc.BindFlags = D3D11_BIND_DECODER | bindFlags;
			textureDesc.CPUAccessFlags = 0;

			ThrowIfFailed(pD3DDevice->CreateTexture2D(&textureDesc, nullptr, &pOutputTexture));

			outputSamples = std::vector<ComPtr<IMFSample>>(numSurfaces);
			outputViews = std::vector<ComPtr<ID3D11VideoDecoderOutputView>>(numSurfaces);

			D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC viewDesc = {};
			viewDesc.DecodeProfile = desiredDecoderProfile;
			viewDesc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;

			for (UINT i = 0; i < numSurfaces; ++i)
			{
				viewDesc.Texture2D.ArraySlice = i;
				ThrowIfFailed(pD3DVideoDevice->CreateVideoDecoderOutputView(pOutputTexture.Get(), &viewDesc, &outputViews[i]));

				ComPtr<IMFMediaBuffer> pBuffer;
				ThrowIfFailed(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), pOutputTexture.Get(), i, FALSE, &pBuffer));

				ComPtr<IMFSample> pSample;
				ThrowIfFailed(MFCreateSample(&pSample));

				ThrowIfFailed(pSample->AddBuffer(pBuffer.Get()));

				outputSamples[i] = pSample;
			}
			/******************************************************************/

			/******************************************************************
			 * Decoding
			 *
			 * https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation#decoding
			 ******************************************************************/
			ThrowIfFailed(pD3DVideoDevice->CreateVideoDecoder(&videoDecoderDesc, &videoDecoderConfigList[0], &pVideoDecoder));
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	void print_stats()
	{
		// Gets the minimum buffer sizes for input and output samples. The MFT will not
		// allocate buffer for input nor output, so we have to do it ourselves and make
		// sure they're the correct sizes
		MFT_INPUT_STREAM_INFO InputStreamInfo = {};
		ThrowIfFailed(pDecoderTransform->GetInputStreamInfo(0, &InputStreamInfo));

		MFT_OUTPUT_STREAM_INFO OutputStreamInfo = {};
		ThrowIfFailed(pDecoderTransform->GetOutputStreamInfo(0, &OutputStreamInfo));

		// There should be three flags set:
		//	1) requires a whole frame be in a single sample
		//	2) requires there be one buffer only in a single sample
		//	3) specifies a fixed sample size (as in cbSize)
		if (InputStreamInfo.dwFlags != 0x7u)
		{
			throw std::exception("Whole Samples, Single Sample Per Buffer, and Fixed Sample Size must be applied");
		}

		// There is an extra 0x100 flag to indicate whether the output
		// stream provides samples. When DXVA is enabled, the decoder
		// will allocate its own samples.
		if ((OutputStreamInfo.dwFlags & 0x7u) == 0)
		{
			throw std::exception("Whole Samples, Single Sample Per Buffer, and Fixed Sample Size must be applied");
		}

		printf("\nInput stream info:\n");
		printf("\tMax latency: %lld\n", InputStreamInfo.hnsMaxLatency);
		printf("\tMin buffer size: %lu\n", InputStreamInfo.cbSize);
		printf("\tMax lookahead: %lu\n", InputStreamInfo.cbMaxLookahead);
		printf("\tAlignment: %lu\n", InputStreamInfo.cbAlignment);

		printf("\nOutput stream info:\n");
		printf("\tFlags: %lu\n", OutputStreamInfo.dwFlags);
		printf("\tMin buffer size: %lu\n", OutputStreamInfo.cbSize);
		printf("\tAlignment: %lu\n", OutputStreamInfo.cbAlignment);

		if (OutputStreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
		{
			printf("\t+---- Output stream provides samples ----+\n\n");
		}
		else
		{
			printf("\t+---- The decoder should allocate its own samples ----+\n\n");
		}
	}

	void decode_loop_cpu()
	{
		// Send messages to decoder to flush data and start streaming
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

		// Start processing frames
		LONGLONG llVideoTimeStamp = 0, llSampleDuration = 0;
		DWORD sampleFlags = 0;

		while (true)
		{
			ComPtr<IMFSample> pVideoSample;
			DWORD streamIndex, flags;
			ThrowIfFailed(pSourceReader->ReadSample(
				MF_SOURCE_READER_FIRST_VIDEO_STREAM,
				0,                              // Flags.
				&streamIndex,                   // Receives the actual stream index. 
				&flags,                         // Receives status flags.
				&llVideoTimeStamp,              // Receives the time stamp.
				&pVideoSample                   // Receives the sample or NULL.
			));

			if (flags & MF_SOURCE_READERF_STREAMTICK)
			{
				printf("\tStream tick.\n");
			}
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
			{
				printf("\tEnd of stream.\n");
				break;
			}

			if (pVideoSample)
			{
				try
				{
					ThrowIfFailed(pVideoSample->SetSampleTime(llVideoTimeStamp));
					ThrowIfFailed(pVideoSample->GetSampleDuration(&llSampleDuration));
					ThrowIfFailed(pVideoSample->GetSampleFlags(&sampleFlags));

					// Gets total length of ALL media buffer samples. We can use here because it's only a
					// single buffer sample copy
					DWORD srcBufferLength;
					ThrowIfFailed(pVideoSample->GetTotalLength(&srcBufferLength));

					// The video processor MFT requires input samples to be allocated by the caller
					ComPtr<IMFSample> pInputSample;
					ThrowIfFailed(MFCreateSample(&pInputSample));

					// Adds a ref count to the pDstBuffer object.
					ComPtr<IMFMediaBuffer> pInputBuffer;
					ThrowIfFailed(MFCreateMemoryBuffer(srcBufferLength, &pInputBuffer));

					// Adds another ref count to the pDstBuffer object.
					ThrowIfFailed(pInputSample->AddBuffer(pInputBuffer.Get()));

					ThrowIfFailed(pVideoSample->CopyAllItems(pInputSample.Get()));
					ThrowIfFailed(pVideoSample->CopyToBuffer(pInputBuffer.Get()));

					// Apply the H264 decoder transform
					ThrowIfFailed(pDecoderTransform->ProcessInput(0, pInputSample.Get(), 0));

					MFT_OUTPUT_STREAM_INFO StreamInfo = {};
					ThrowIfFailed(pDecoderTransform->GetOutputStreamInfo(0, &StreamInfo));

					MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
					outputDataBuffer.dwStreamID = 0;
					outputDataBuffer.dwStatus = 0;
					outputDataBuffer.pEvents = NULL;
					outputDataBuffer.pSample = NULL;

					DWORD mftProccessStatus = 0;
					HRESULT mftProcessOutput = S_OK;

					// If the decoder returns MF_E_NOTACCEPTING then it means that it has enough
					// data to produce one or more output samples.
					// Here, we generate new output by calling IMFTransform::ProcessOutput until it
					// results in MF_E_TRANSFORM_NEED_MORE_INPUT which breaks out of the loop.
					while (mftProcessOutput == S_OK)
					{
						ComPtr<IMFSample> pH264DecodeOutSample;

						if ((StreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
						{
							ThrowIfFailed(MFCreateSample(&pH264DecodeOutSample));

							ComPtr<IMFMediaBuffer> pBuffer;
							ThrowIfFailed(MFCreateMemoryBuffer(StreamInfo.cbSize, &pBuffer));

							ThrowIfFailed(pH264DecodeOutSample->AddBuffer(pBuffer.Get()));

							outputDataBuffer.pSample = pH264DecodeOutSample.Get();
						}

						mftProcessOutput = pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);

						printf("Process output result %.2X, MFT status %.2X.\n", mftProcessOutput, mftProccessStatus);

						if (mftProcessOutput == S_OK)
						{
							// Write the decoded sample to the nv12 texture
							ComPtr<IMFMediaBuffer> buffer;
							ThrowIfFailed(outputDataBuffer.pSample->GetBufferByIndex(0, &buffer));

							DWORD bufferLength;
							ThrowIfFailed(buffer->GetCurrentLength(&bufferLength));

							//printf("Sample size %i.\n", bufferLength);

							byte* byteBuffer = NULL;
							DWORD maxLength = 0, currentLength = 0;
							ThrowIfFailed(buffer->Lock(&byteBuffer, &maxLength, &currentLength));

							nv12_tex_set_buffer(_nv12_tex, byteBuffer);

							ThrowIfFailed(buffer->Unlock());
						}

						// More input is not an error condition but it means the allocated output sample is empty
						if (mftProcessOutput != S_OK && mftProcessOutput != MF_E_TRANSFORM_NEED_MORE_INPUT)
						{
							printf("MFT ProcessOutput error result %.2X, MFT status %.2X.\n", mftProcessOutput, mftProccessStatus);
							throw std::exception("Error getting H264 decoder transform output, error code %.2X.\n", mftProcessOutput);
						}
					}
				}
				catch (const std::exception& e)
				{
					log_err(e.what());
					throw e;
				}
			}
		}

		shutdown();
	}

	void decode_loop_gpu()
	{
		// Send messages to decoder to flush data and start streaming
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

		// Start processing frames
		LONGLONG llVideoTimeStamp = 0, llSampleDuration = 0;
		DWORD sampleFlags = 0;

		while (true)
		{
			ComPtr<IMFSample> pVideoSample;
			DWORD streamIndex, flags;
			ThrowIfFailed(pSourceReader->ReadSample(
				MF_SOURCE_READER_FIRST_VIDEO_STREAM,
				0,                              // Flags.
				&streamIndex,                   // Receives the actual stream index. 
				&flags,                         // Receives status flags.
				&llVideoTimeStamp,              // Receives the time stamp.
				&pVideoSample                   // Receives the sample or NULL.
			));

			if (flags & MF_SOURCE_READERF_STREAMTICK)
			{
				printf("\tStream tick.\n");
			}
			if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
			{
				printf("\tEnd of stream.\n");
				break;
			}

			if (pVideoSample)
			{
				try
				{
					/******************************************************************
					 * Decoding (continued)
					 *
					 * https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation#decoding
					 ******************************************************************/
					 // On each frame, call IMFDXGIDeviceManager::TestDevice to test the availability of the DXGI
					if (FAILED(pDXGIDeviceManager->TestDevice(deviceHandle)))
					{
						// If the device has changed, the software decoder must recreate the decoder device
						ThrowIfFailed(pDXGIDeviceManager->CloseDeviceHandle(deviceHandle));
						
						// Release all resources associated with the previous Direct3D 11 device
						release_dx_resources();
						
						// Open a new device handle, negotiate a new decoder configuration, and create a new decoder device
						create_dx_video_decoder();
					}

					ThrowIfFailed(pVideoSample->SetSampleTime(llVideoTimeStamp));
					ThrowIfFailed(pVideoSample->GetSampleDuration(&llSampleDuration));
					ThrowIfFailed(pVideoSample->GetSampleFlags(&sampleFlags));

				}
				catch (const std::exception& e)
				{
					log_err(e.what());
					throw e;
				}
			}
		}

		shutdown();
	}

	void shutdown()
	{
		MFShutdown();

		pSourceReader.Reset();
		pFileVideoMediaType.Reset();
		pDecoderTransform.Reset();
		pDXGIDeviceManager.Reset();

		// GPU-specific fields
		release_dx_resources();

		if (ppActivate && *ppActivate)
		{
			CoTaskMemFree(ppActivate);
		}
	}

	void release_dx_resources()
	{
		pVideoContext.Reset();
		pVideoDecoder.Reset();
		pOutputTexture.Reset();
		outputSamples.clear();
		outputViews.clear();
	}
} // namespace nakamir