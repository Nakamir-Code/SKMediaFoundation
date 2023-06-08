#include "mf_decoder.h"
#include "mf_utility.h"
#include "error.h"
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <d3d11.h>
#include <stereokit.h>
#include <wrl/client.h>

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace nakamir {

	static void mf_validate_stream_info(/**[in]**/ IMFTransform* pDecoderTransform);

	_VIDEO_DECODER mf_create_mft_software_decoder(IMFMediaType* pInputMediaType, IMFMediaType* pOutputMediaType, IMFTransform** ppDecoderTransform, IMFActivate*** pppActivate)
	{
		_VIDEO_DECODER video_decoder = SOFTWARE_MFT_VIDEO_DECODER;
		try
		{
			GUID inputMajorType = {};
			GUID inputSubType = {};
			ThrowIfFailed(pInputMediaType->GetGUID(MF_MT_MAJOR_TYPE, &inputMajorType));
			ThrowIfFailed(pInputMediaType->GetGUID(MF_MT_SUBTYPE, &inputSubType));

			MFT_REGISTER_TYPE_INFO inputType = {};
			inputType.guidMajorType = inputMajorType;
			inputType.guidSubtype = inputSubType;

			GUID outputMajorType = {};
			GUID outputSubType = {};
			ThrowIfFailed(pOutputMediaType->GetGUID(MF_MT_MAJOR_TYPE, &outputMajorType));
			ThrowIfFailed(pOutputMediaType->GetGUID(MF_MT_SUBTYPE, &outputSubType));

			MFT_REGISTER_TYPE_INFO outputType = {};
			outputType.guidMajorType = outputMajorType;
			outputType.guidSubtype = outputSubType;

			// Enumerate decoders that match the input type
			UINT32 count = 0;
			ThrowIfFailed(MFTEnumEx(
				MFT_CATEGORY_VIDEO_DECODER,
				MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
				&inputType,
				&outputType,
				pppActivate,
				&count
			));

			// Check if any decoders were found
			if (count <= 0)
			{
				throw std::exception("No decoders found! :(");
			}

			printf("\nDECODERS FOUND:\n");

			for (UINT32 i = 0; i < count; i++)
			{
				LPWSTR pszName = nullptr;
				UINT32 pszLength;
				if (FAILED((*pppActivate)[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &pszName, &pszLength)) || !pszName)
				{
					continue;
				}

				printf("\t- ");
				if (i == 0) printf("[");
				wprintf(pszName);
				if (i == 0) printf("]");
				printf("\n");
				CoTaskMemFree(pszName);
			}

			// Activate first decoder object and get a pointer to it
			ThrowIfFailed((*pppActivate)[0]->ActivateObject(IID_PPV_ARGS(ppDecoderTransform)));

			ComPtr<IMFAttributes> pAttributes;
			ThrowIfFailed((*ppDecoderTransform)->GetAttributes(pAttributes.GetAddressOf()));

			// This attribute does not affect hardware-accelerated video decoding that uses DirectX Video Acceleration (DXVA)
			ThrowIfFailed(pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

			// The decoder MFT must expose the MF_SA_D3D_AWARE attribute to TRUE
			UINT32 isD3DAware = false;
			ThrowIfFailed(pAttributes->GetUINT32(MF_SA_D3D_AWARE, &isD3DAware));
			if (isD3DAware)
			{
				try
				{
					// Create the DXGI Device Manager
					UINT resetToken;
					ComPtr<IMFDXGIDeviceManager> pDXGIDeviceManager;
					ThrowIfFailed(MFCreateDXGIDeviceManager(&resetToken, pDXGIDeviceManager.GetAddressOf()));

					// Set the Direct3D 11 device on the DXGI Device Manager
					ID3D11Device* pD3DDevice = (ID3D11Device*)backend_d3d11_get_d3d_device();
					ThrowIfFailed(pDXGIDeviceManager->ResetDevice(pD3DDevice, resetToken));

					// The Topology Loader calls IMFTransform::ProcessMessage with the MFT_MESSAGE_SET_D3D_MANAGER message
					ThrowIfFailed((*ppDecoderTransform)->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(pDXGIDeviceManager.Get())));

					// It is recommended that you use multi-thread protection on the device context to prevent deadlock issues 
					ComPtr<ID3D10Multithread> pMultiThread;
					ThrowIfFailed(pD3DDevice->QueryInterface(__uuidof(ID3D10Multithread), (void**)pMultiThread.GetAddressOf()));
					pMultiThread->SetMultithreadProtected(TRUE);

					video_decoder = D3D11_MFT_VIDEO_DECODER;

					log_info("Video decoder is hardware accelerated through DXVA");
				}
				catch (const std::exception& e)
				{
					log_err(e.what());
				}
			}

			if (video_decoder != D3D11_MFT_VIDEO_DECODER)
			{
				log_warn("Video decoder is not D3D aware, decoding may be slow.");
				ThrowIfFailed((*ppDecoderTransform)->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, NULL));
			}

			// Set the input media type
			ThrowIfFailed((*ppDecoderTransform)->SetInputType(0, pInputMediaType, 0));

			// Set the output media type
			ThrowIfFailed((*ppDecoderTransform)->SetOutputType(0, pOutputMediaType, 0));

			mf_validate_stream_info(*ppDecoderTransform);
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}

		return video_decoder;
	}

	static void mf_validate_stream_info(IMFTransform* pDecoderTransform)
	{
		try
		{
			// Gets the minimum buffer sizes for input and output samples
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
			// will allocate its own samples
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
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	void mf_decode_sample_to_buffer(IMFSample* pVideoSample, IMFTransform* pDecoderTransform, const std::function<void(byte*)>& onReceiveImageBuffer)
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
			ThrowIfFailed(MFCreateSample(pInputSample.GetAddressOf()));

			// Adds a ref count to the pDstBuffer object.
			ComPtr<IMFMediaBuffer> pInputBuffer;
			ThrowIfFailed(MFCreateMemoryBuffer(srcBufferLength, pInputBuffer.GetAddressOf()));

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
				IMFSample* pDecodedOutSample;

				if ((StreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
				{
					ThrowIfFailed(MFCreateSample(&pDecodedOutSample));

					ComPtr<IMFMediaBuffer> pBuffer;
					ThrowIfFailed(MFCreateMemoryBuffer(StreamInfo.cbSize, pBuffer.GetAddressOf()));
					ThrowIfFailed(pDecodedOutSample->AddBuffer(pBuffer.Get()));

					outputDataBuffer.pSample = pDecodedOutSample;
				}

				mftProcessOutput = pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);

				if (outputDataBuffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)
				{
					// Get the new media type for the stream
					ComPtr<IMFMediaType> pNewMediaType;
					ThrowIfFailed(pDecoderTransform->GetOutputAvailableType(0, 0, pNewMediaType.GetAddressOf()));
					ThrowIfFailed(pDecoderTransform->SetOutputType(0, pNewMediaType.Get(), 0));

					ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));

					mftProcessOutput = pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);
				}

				//printf("Process output result %.2X, MFT status %.2X.\n", mftProcessOutput, mftProccessStatus);

				if (mftProcessOutput == S_OK)
				{
					// Write the decoded sample to the nv12 texture
					ComPtr<IMFMediaBuffer> buffer;
					ThrowIfFailed(outputDataBuffer.pSample->GetBufferByIndex(0, buffer.GetAddressOf()));

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