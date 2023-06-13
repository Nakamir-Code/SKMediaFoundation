#include "mf_video_decoder.h"
#include "mf_utility.h"
#include "error.h"
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <d3d11.h>
#include <wrl/client.h>
// UWP requires a different header for ICodecAPI: https://learn.microsoft.com/en-us/windows/win32/api/strmif/nn-strmif-icodecapi
#ifdef WINDOWS_UWP
#include <icodecapi.h>
#endif

using Microsoft::WRL::ComPtr;

namespace nakamir {

	static void mf_validate_stream_info(/**[in]**/ IMFTransform* pDecoderTransform);

	_VIDEO_DECODER mf_create_mft_video_decoder(IMFMediaType* pInputMediaType, IMFMediaType* pOutputMediaType, IMFTransform** ppDecoderTransform, IMFActivate*** pppActivate)
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

			log_info("DECODERS FOUND:");

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

			// Activate first decoder object and get a pointer to it
			ThrowIfFailed((*pppActivate)[0]->ActivateObject(IID_PPV_ARGS(ppDecoderTransform)));

			ComPtr<IMFAttributes> pAttributes;
			ThrowIfFailed((*ppDecoderTransform)->GetAttributes(pAttributes.GetAddressOf()));

			// This attribute does not affect hardware-accelerated video decoding that uses DirectX Video Acceleration (DXVA)
			ThrowIfFailed(pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

			// Set the hardware decoding parameters
			ComPtr<ICodecAPI> pCodecAPI;
			ThrowIfFailed((*ppDecoderTransform)->QueryInterface(IID_PPV_ARGS(pCodecAPI.GetAddressOf())));

			VARIANT variant = {};
			variant.vt = VT_UI4;
			variant.ulVal = 1;
			ThrowIfFailed(pCodecAPI->SetValue(&CODECAPI_AVLowLatencyMode, &variant));

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
				log_info("\t+---- The decoder should allocate its own samples ----+");
			}
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}
} // namespace nakamir