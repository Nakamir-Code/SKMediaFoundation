#include "mf_encoder.h"
#include "mf_utility.h"
#include "error.h"
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <d3d11.h>
#include <wrl/client.h>
// UWP requires a different header for ICodecAPI: https://learn.microsoft.com/en-us/windows/win32/api/strmif/nn-strmif-icodecapi
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
#include <icodecapi.h>
#endif

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace nakamir {

	static void mf_validate_stream_info(/**[in]**/ IMFTransform* pEncoderTransform);

	void mf_create_mft_software_encoder(IMFMediaType* pInputMediaType, IMFMediaType* pOutputMediaType, IMFTransform** ppEncoderTransform, IMFActivate*** pppActivate)
	{
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

			// Enumerate encoders that match the input type
			UINT32 count = 0;
			ThrowIfFailed(MFTEnumEx(
				MFT_CATEGORY_VIDEO_ENCODER,
				(MFT_ENUM_FLAG_HARDWARE & MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER,
				&inputType,
				&outputType,
				pppActivate,
				&count
			));

			// Check if any encoders were found
			if (count <= 0)
			{
				throw std::exception("No hardware encoders found! :(");
			}

			printf("\nENCODERS FOUND:\n");

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

			// Activate first encoder object and get a pointer to it
			ThrowIfFailed((*pppActivate)[0]->ActivateObject(IID_PPV_ARGS(ppEncoderTransform)));

			ComPtr<IMFAttributes> pAttributes;
			ThrowIfFailed((*ppEncoderTransform)->GetAttributes(pAttributes.GetAddressOf()));

			// This attribute does not affect hardware-accelerated video encoding that uses DirectX Video Acceleration (DXVA)
			ThrowIfFailed(pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

			// Set the hardware encoding parameters
			ComPtr<ICodecAPI> pCodecAPI;
			ThrowIfFailed((*ppEncoderTransform)->QueryInterface(IID_PPV_ARGS(pCodecAPI.GetAddressOf())));

			VARIANT variant = {};
			variant.vt = VT_UI4;
			variant.ulVal = eAVEncCommonRateControlMode_CBR;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVEncCommonRateControlMode, &variant));

			UINT32 bitrate;
			ThrowIfFailed(pOutputMediaType->GetUINT32(MF_MT_AVG_BITRATE, &bitrate));
			variant.ulVal = bitrate;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &variant));

			variant.ulVal = eAVEncAdaptiveMode_Resolution;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVEncAdaptiveMode, &variant));

			variant.vt = VT_BOOL;
			variant.boolVal = VARIANT_TRUE;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &variant));

			// Set the output media type
			ThrowIfFailed((*ppEncoderTransform)->SetOutputType(0, pOutputMediaType, 0));

			// Set the input media type
			ThrowIfFailed((*ppEncoderTransform)->SetInputType(0, pInputMediaType, 0));

			mf_validate_stream_info(*ppEncoderTransform);
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	static void mf_validate_stream_info(IMFTransform* pEncoderTransform)
	{
		try
		{
			// Gets the minimum buffer sizes for input and output samples. The MFT will not
			// allocate buffer for input nor output, so we have to do it ourselves and make
			// sure they're the correct sizes
			MFT_INPUT_STREAM_INFO InputStreamInfo = {};
			ThrowIfFailed(pEncoderTransform->GetInputStreamInfo(0, &InputStreamInfo));

			MFT_OUTPUT_STREAM_INFO OutputStreamInfo = {};
			ThrowIfFailed(pEncoderTransform->GetOutputStreamInfo(0, &OutputStreamInfo));

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
				printf("\t+---- The encoder should allocate its own samples ----+\n\n");
			}
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	void mf_encode_sample_to_buffer(IMFSample* pVideoSample, IMFTransform* pEncoderTransform, const std::function<void(uint32_t, byte*)>& onReceiveEncodedBuffer)
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

			// Apply the H264 encoder transform
			ThrowIfFailed(pEncoderTransform->ProcessInput(0, pInputSample.Get(), 0));

			MFT_OUTPUT_STREAM_INFO StreamInfo = {};
			ThrowIfFailed(pEncoderTransform->GetOutputStreamInfo(0, &StreamInfo));

			MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
			outputDataBuffer.dwStreamID = 0;
			outputDataBuffer.dwStatus = 0;
			outputDataBuffer.pEvents = NULL;
			outputDataBuffer.pSample = NULL;

			DWORD mftProccessStatus = 0;
			HRESULT mftProcessOutput = S_OK;

			// If the encoder returns MF_E_NOTACCEPTING then it means that it has enough
			// data to produce one or more output samples.
			// Here, we generate new output by calling IMFTransform::ProcessOutput until it
			// results in MF_E_TRANSFORM_NEED_MORE_INPUT which breaks out of the loop.
			while (mftProcessOutput == S_OK)
			{
				IMFSample* pEncodedOutSample;

				if ((StreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
				{
					ThrowIfFailed(MFCreateSample(&pEncodedOutSample));

					ComPtr<IMFMediaBuffer> pBuffer;
					ThrowIfFailed(MFCreateMemoryBuffer(StreamInfo.cbSize, pBuffer.GetAddressOf()));
					ThrowIfFailed(pEncodedOutSample->AddBuffer(pBuffer.Get()));

					outputDataBuffer.pSample = pEncodedOutSample;
				}

				mftProcessOutput = pEncoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);

				if (outputDataBuffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)
				{
					// Get the new media type for the stream
					ComPtr<IMFMediaType> pNewMediaType;
					ThrowIfFailed(pEncoderTransform->GetOutputAvailableType(0, 0, pNewMediaType.GetAddressOf()));
					ThrowIfFailed(pEncoderTransform->SetOutputType(0, pNewMediaType.Get(), 0));

					ThrowIfFailed(pEncoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));

					mftProcessOutput = pEncoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);
				}

				//printf("Process output result %.2X, MFT status %.2X.\n", mftProcessOutput, mftProccessStatus);

				if (mftProcessOutput == S_OK)
				{
					// Write the encoded sample to the nv12 texture
					ComPtr<IMFMediaBuffer> buffer;
					ThrowIfFailed(outputDataBuffer.pSample->GetBufferByIndex(0, buffer.GetAddressOf()));

					DWORD bufferLength;
					ThrowIfFailed(buffer->GetCurrentLength(&bufferLength));

					printf("Sample size %i.\n", bufferLength);

					byte* byteBuffer = NULL;
					DWORD maxLength = 0, currentLength = 0;
					ThrowIfFailed(buffer->Lock(&byteBuffer, &maxLength, &currentLength));
					onReceiveEncodedBuffer(currentLength, byteBuffer);
					ThrowIfFailed(buffer->Unlock());

					printf("Output size %i.\n", currentLength);

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
					throw std::exception("Error getting H264 encoder transform output, error code %.2X.\n", mftProcessOutput);
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