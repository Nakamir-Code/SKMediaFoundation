#include "mf_decoder.h"
#include "error.h"
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <codecapi.h>
#include <d3d11.h>
#include <thread>

EXTERN_GUID(D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 0x1b81be68, 0xa0c7, 0x11d3, 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5);

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace nakamir {

	_VIDEO_DECODER mf_create_mft_software_decoder(IMFMediaType* pInputMediaType, /**[out]**/ IMFTransform** ppDecoderTransform, /**[out]**/ IMFActivate*** pppActivate)
	{
		_VIDEO_DECODER video_decoder = SOFTWARE_MFT_VIDEO_DECODER;
		try
		{
			// Get the major and subtype of the mp4 video
			GUID majorType = {};
			GUID subType = {};
			ThrowIfFailed(pInputMediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType));
			ThrowIfFailed(pInputMediaType->GetGUID(MF_MT_SUBTYPE, &subType));

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
				pppActivate,
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
				if (FAILED((*pppActivate)[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &pszName, &pszLength)))
				{
					continue;
				}

				OutputDebugStringW(pszName);
				CoTaskMemFree(pszName);
			}

			// Activate first decoder object and get a pointer to it
			ThrowIfFailed((*pppActivate)[0]->ActivateObject(IID_PPV_ARGS(ppDecoderTransform)));

			ComPtr<IMFAttributes> pAttributes;
			ThrowIfFailed((*ppDecoderTransform)->GetAttributes(&pAttributes));

			// Tell the decoder to allocate resources for the maximum resolution in
			// order to minimize glitching on resolution changes.
			ThrowIfFailed(pAttributes->SetUINT32(MF_MT_DECODER_USE_MAX_RESOLUTION, 1));

			// This attribute does not affect hardware-accelerated video decoding that uses DirectX Video Acceleration (DXVA)
			ThrowIfFailed(pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

			// The decoder MFT must expose the MF_SA_D3D_AWARE attribute to TRUE
			UINT32 isD3DAware = false;
			ThrowIfFailed(pAttributes->GetUINT32(MF_SA_D3D_AWARE, &isD3DAware));
			if (isD3DAware)
			{
				try
				{
					ThrowIfFailed(pAttributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE));

					// Create the DXGI Device Manager
					UINT resetToken;
					ComPtr<IMFDXGIDeviceManager> pDXGIDeviceManager;
					ThrowIfFailed(MFCreateDXGIDeviceManager(&resetToken, &pDXGIDeviceManager));

					// Set the Direct3D 11 device on the DXGI Device Manager
					ID3D11Device* pD3DDevice = (ID3D11Device*)backend_d3d11_get_d3d_device();
					ThrowIfFailed(pDXGIDeviceManager->ResetDevice(pD3DDevice, resetToken));

					// The Topology Loader calls IMFTransform::ProcessMessage with the MFT_MESSAGE_SET_D3D_MANAGER message
					ThrowIfFailed((*ppDecoderTransform)->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(pDXGIDeviceManager.Get())));

					// It is recommended that you use multi-thread protection on the device context to prevent deadlock issues 
					ComPtr<ID3D10Multithread> pMultiThread;
					ThrowIfFailed(pD3DDevice->QueryInterface(__uuidof(ID3D10Multithread), (void**)&pMultiThread));
					pMultiThread->SetMultithreadProtected(TRUE);

					video_decoder = D3D_VIDEO_DECODER;
				}
				catch (const std::exception& e)
				{
					log_err(e.what());
				}
			}

			if (video_decoder != D3D_VIDEO_DECODER)
			{
				log_warn("Video decoder is not D3D aware, decoding may be slow.");
				ThrowIfFailed((*ppDecoderTransform)->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, NULL));
			}

			// Create input media type for decoder and copy all items from file video media type
			ThrowIfFailed((*ppDecoderTransform)->SetInputType(0, pInputMediaType, 0));

			// Create output media type from the decoder
			ComPtr<IMFMediaType> pOutputMediaType;
			BOOL isOutputTypeConfigured = FALSE;
			for (int i = 0; SUCCEEDED((*ppDecoderTransform)->GetOutputAvailableType(0, i, &pOutputMediaType)); ++i)
			{
				GUID outSubtype = {};
				ThrowIfFailed(pOutputMediaType->GetGUID(MF_MT_SUBTYPE, &outSubtype));

				if (outSubtype == MFVideoFormat_NV12)
				{
					ThrowIfFailed((*ppDecoderTransform)->SetOutputType(0, pOutputMediaType.Get(), 0));
					isOutputTypeConfigured = TRUE;
					break;
				}
			}

			if (!isOutputTypeConfigured)
			{
				throw std::exception("Failed to find an output type with an NV12 subtype.");
			}

			ThrowIfFailed((*ppDecoderTransform)->SetOutputType(0, pOutputMediaType.Get(), 0));
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}

		return video_decoder;
	}

	void mf_print_stream_info(IMFTransform* pDecoderTransform)
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

	void mf_decode_sample_cpu(IMFSample* pVideoSample, IMFTransform* pDecoderTransform, std::function<void(byte*)> onReceiveImageBuffer)
	{
		// Start processing the frame
		LONGLONG llSampleTime = 0, llSampleDuration = 0;
		DWORD sampleFlags = 0;

		try
		{
			ThrowIfFailed(pVideoSample->GetSampleTime(&llSampleTime));
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
				IMFSample* pH264DecodeOutSample;

				if ((StreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
				{
					ThrowIfFailed(MFCreateSample(&pH264DecodeOutSample));

					ComPtr<IMFMediaBuffer> pBuffer;
					ThrowIfFailed(MFCreateMemoryBuffer(StreamInfo.cbSize, &pBuffer));
					ThrowIfFailed(pH264DecodeOutSample->AddBuffer(pBuffer.Get()));

					outputDataBuffer.pSample = pH264DecodeOutSample;
				}

				mftProcessOutput = pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);

				if (outputDataBuffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)
				{
					// Get the new media type for the stream
					ComPtr<IMFMediaType> pNewMediaType;
					ThrowIfFailed(pDecoderTransform->GetOutputAvailableType(0, 0, &pNewMediaType));
					ThrowIfFailed(pDecoderTransform->SetOutputType(0, pNewMediaType.Get(), 0));

					ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));

					mftProcessOutput = pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);
				}

				//printf("Process output result %.2X, MFT status %.2X.\n", mftProcessOutput, mftProccessStatus);

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
					onReceiveImageBuffer(byteBuffer);
					ThrowIfFailed(buffer->Unlock());

					// Release sample as it is not processed any further.
					if (outputDataBuffer.pSample)
						outputDataBuffer.pSample->Release();
					if (outputDataBuffer.pEvents)
						outputDataBuffer.pEvents->Release();
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
} // namespace nakamir