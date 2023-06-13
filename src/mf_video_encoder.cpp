#include "mf_video_encoder.h"
#include "mf_utility.h"
#include "error.h"
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl/client.h>
// UWP requires a different header for ICodecAPI: https://learn.microsoft.com/en-us/windows/win32/api/strmif/nn-strmif-icodecapi
#ifdef WINDOWS_UWP
#include <icodecapi.h>
#endif

using Microsoft::WRL::ComPtr;

namespace nakamir {
	static void mf_validate_stream_info(/**[in]**/ IMFTransform* pEncoderTransform);

	void mf_create_mft_video_encoder(IMFMediaType* pInputMediaType, IMFMediaType* pOutputMediaType, IMFTransform** ppEncoderTransform, IMFActivate*** pppActivate)
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

			log_info("ENCODERS FOUND:");

			for (UINT32 i = 0; i < count; i++)
			{
				LPWSTR pszName = nullptr;
				UINT32 pszLength;
				if (FAILED((*pppActivate)[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &pszName, &pszLength)) || !pszName)
				{
					continue;
				}

				int requiredSize = WideCharToMultiByte(CP_UTF8, 0, pszName, -1, nullptr, 0, nullptr, nullptr);
				char* pszNameBuffer = new char[requiredSize];
				WideCharToMultiByte(CP_UTF8, 0, pszName, -1, pszNameBuffer, requiredSize, nullptr, nullptr);

				std::string result = "\t- ";
				if (i == 0) result += "[";
				result += pszNameBuffer;
				if (i == 0) result += "]";
				log_info(result.c_str());

				delete[] pszNameBuffer;
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

			variant.ulVal = eAVScenarioInfo_LiveStreaming; // eAVScenarioInfo_LiveStreaming, eAVScenarioInfo_CameraRecord
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVScenarioInfo, &variant));

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

			log_info("Input stream info:");
			log_info(std::format("\tMax latency: {}", InputStreamInfo.hnsMaxLatency).c_str());
			log_info(std::format("\tMin buffer size: {}", InputStreamInfo.cbSize).c_str());
			log_info(std::format("\tMax lookahead: {}", InputStreamInfo.cbMaxLookahead).c_str());
			log_info(std::format("\tAlignment: {}", InputStreamInfo.cbAlignment).c_str());

			log_info("Output stream info:");
			log_info(std::format("\tFlags: {}", OutputStreamInfo.dwFlags).c_str());
			log_info(std::format("\tMin buffer size: {}", OutputStreamInfo.cbSize).c_str());
			log_info(std::format("\tAlignment: {}", OutputStreamInfo.cbAlignment).c_str());

			if (OutputStreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
			{
				log_info("\t+---- Output stream provides samples ----+");
			}
			else
			{
				log_info("\t+---- The encoder should allocate its own samples ----+");
			}
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}
} // namespace nakamir