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

	void mf_create_mft_software_encoder(const GUID& targetSubType, IMFMediaType** ppInputMediaType, IMFMediaType** ppOutputMediaType, IMFTransform** ppEncoderTransform, IMFActivate*** pppActivate)
	{
		try
		{
			const UINT32 bitrate = 5000000;
			UINT32 width, height, fps, den;
			ThrowIfFailed(MFGetAttributeSize(*ppInputMediaType, MF_MT_FRAME_SIZE, &width, &height));
			ThrowIfFailed(MFGetAttributeRatio(*ppInputMediaType, MF_MT_FRAME_RATE, &fps, &den));

			// Get the major and subtype of the mp4 video
			GUID majorType = {};
			GUID subType = {};
			ThrowIfFailed((*ppInputMediaType)->GetGUID(MF_MT_MAJOR_TYPE, &majorType));
			ThrowIfFailed((*ppInputMediaType)->GetGUID(MF_MT_SUBTYPE, &subType));

			// Create H.264 encoder.
			MFT_REGISTER_TYPE_INFO inputType = {};
			inputType.guidMajorType = majorType;
			inputType.guidSubtype = MFVideoFormat_NV12;// subType; // TODO: comment back out

			MFT_REGISTER_TYPE_INFO outputType = {};
			outputType.guidMajorType = MFMediaType_Video;
			outputType.guidSubtype = targetSubType;

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

			variant.ulVal = bitrate;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &variant));

			variant.ulVal = eAVEncAdaptiveMode_Resolution;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVEncAdaptiveMode, &variant));

			variant.vt = VT_BOOL;
			variant.boolVal = VARIANT_TRUE;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &variant));

			// Create output media type from the encoder
			ThrowIfFailed((*ppEncoderTransform)->GetOutputAvailableType(0, 0, ppOutputMediaType));
			mf_set_default_media_type(*ppOutputMediaType, targetSubType, bitrate, width, height, fps);
			ThrowIfFailed((*ppEncoderTransform)->SetOutputType(0, *ppOutputMediaType, 0));

			// Create input media type for the encoder
			ThrowIfFailed((*ppEncoderTransform)->GetInputAvailableType(0, 0, ppInputMediaType));
			mf_set_default_media_type(*ppInputMediaType, targetSubType, bitrate, width, height, fps);
			ThrowIfFailed((*ppEncoderTransform)->SetInputType(0, *ppInputMediaType, 0));

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
} // namespace nakamir