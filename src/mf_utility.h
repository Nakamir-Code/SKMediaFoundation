#pragma once

#include "error.h"
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <format>
#include <stereokit.h>

using namespace sk;
using Microsoft::WRL::ComPtr;

namespace nakamir {

	enum _MFT_TYPE
	{
		UNKNOWN_MFT_TYPE,
		SOFTWARE_SYNC_MFT_TYPE,
		SOFTWARE_ASYNC_MFT_TYPE,
		D3D11_SYNC_MFT_TYPE,
		D3D11_ASYNC_MFT_TYPE,
	};

	static void mf_set_default_media_type(/**[in]**/ IMFMediaType* pMediaType, const GUID& subType, UINT32 bitrate, UINT32 width, UINT32 height, UINT32 fps)
	{
		try
		{
			// Input media type settings
			ThrowIfFailed(pMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			ThrowIfFailed(pMediaType->SetGUID(MF_MT_SUBTYPE, subType));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_AVG_BITRATE, bitrate));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Normal));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
			ThrowIfFailed(MFSetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, width, height));
			ThrowIfFailed(MFSetAttributeRatio(pMediaType, MF_MT_FRAME_RATE, fps, 1));
			ThrowIfFailed(MFSetAttributeRatio(pMediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	static void mf_transform_sample_to_buffer(/**[in]**/ IMFTransform* pTransform, /**[in]**/ IMFSample* pVideoSample, /**[in]**/ void(*onReceiveBuffer)(IMFTransform*, IMFSample*, void*), /**[in]**/ void* pContext = nullptr)
	{
		try
		{
			HRESULT mftProcessOutput = S_OK;

			ComPtr<IMFMediaEventGenerator> pEventGen;
			HRESULT hr = pTransform->QueryInterface(IID_PPV_ARGS(pEventGen.GetAddressOf()));
			if (SUCCEEDED(hr))
			{
				ComPtr<IMFMediaEventGenerator> pEventGen;
				ThrowIfFailed(pTransform->QueryInterface(IID_PPV_ARGS(pEventGen.GetAddressOf())));

				ComPtr<IMFMediaEvent> pEvent;
				ThrowIfFailed(pEventGen->GetEvent(0, pEvent.GetAddressOf()));

				MediaEventType eventType;
				ThrowIfFailed(pEvent->GetType(&eventType));

				if (eventType == METransformNeedInput)
				{
					ThrowIfFailed(pTransform->ProcessInput(0, pVideoSample, 0));
				}

				if (eventType != METransformHaveOutput)
				{
					mftProcessOutput = S_FALSE;
				}
			}
			else
			{
				ThrowIfFailed(pTransform->ProcessInput(0, pVideoSample, 0));
			}

			MFT_OUTPUT_STREAM_INFO StreamInfo = {};
			ThrowIfFailed(pTransform->GetOutputStreamInfo(0, &StreamInfo));

			MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
			outputDataBuffer.dwStreamID = 0;
			outputDataBuffer.dwStatus = 0;
			outputDataBuffer.pEvents = NULL;
			outputDataBuffer.pSample = NULL;

			DWORD mftProccessStatus = 0;

			ComPtr<IMFSample> pOutSample;

			// If the transform returns MF_E_NOTACCEPTING then it means that it has enough
			// data to produce one or more output samples.
			// Here, we generate new output by calling IMFTransform::ProcessOutput until it
			// results in MF_E_TRANSFORM_NEED_MORE_INPUT which breaks out of the loop.
			while (mftProcessOutput == S_OK)
			{
				if ((StreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
				{
					ThrowIfFailed(MFCreateSample(pOutSample.ReleaseAndGetAddressOf()));

					ComPtr<IMFMediaBuffer> pBuffer;
					ThrowIfFailed(MFCreateMemoryBuffer(StreamInfo.cbSize, pBuffer.GetAddressOf()));
					ThrowIfFailed(pOutSample->AddBuffer(pBuffer.Get()));

					outputDataBuffer.pSample = pOutSample.Get();
				}

				mftProcessOutput = pTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);

				if (outputDataBuffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)
				{
					// Get the new media type for the stream
					ComPtr<IMFMediaType> pNewMediaType;
					ThrowIfFailed(pTransform->GetOutputAvailableType(0, 0, pNewMediaType.GetAddressOf()));
					ThrowIfFailed(pTransform->SetOutputType(0, pNewMediaType.Get(), 0));

					ThrowIfFailed(pTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));

					mftProcessOutput = pTransform->ProcessOutput(0, 1, &outputDataBuffer, &mftProccessStatus);
				}

				//char buffer[1024];
				//std::snprintf(buffer, sizeof(buffer), "MFT ProcessOutput error result %.2X, MFT status %.2X", mftProcessOutput, mftProccessStatus);
				//log_info(buffer);

				if (mftProcessOutput == S_OK)
				{
					onReceiveBuffer(pTransform, outputDataBuffer.pSample, pContext);

					// Release the completed sample if our smart pointer doesn't do it for us
					if (outputDataBuffer.pSample && !pOutSample)
						outputDataBuffer.pSample->Release();
					if (outputDataBuffer.pEvents)
						outputDataBuffer.pEvents->Release();
					break;
				}

				// More input is not an error condition but it means the allocated output sample is empty
				if (mftProcessOutput != S_OK && mftProcessOutput != MF_E_TRANSFORM_NEED_MORE_INPUT)
				{
					throw std::exception("Error getting transform output, error code %.2X", mftProcessOutput);
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