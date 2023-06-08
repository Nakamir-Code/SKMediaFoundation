#include "../mf_examples.h"
#include "../mf_encoder.h"
#include "../mf_decoder.h"
#include "../mf_utility.h"
#include "../error.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <codecapi.h>

using Microsoft::WRL::ComPtr;

namespace nakamir {

	static IMFActivate** ppActivate = NULL;
	static ComPtr<IMFTransform> pEncoderTransform;
	static ComPtr<IMFTransform> pDecoderTransform;

	static nv12_tex_t _nv12_tex;

	// PRIVATE METHODS
	static void mf_roundtrip_mediacapture_shutdown();

	void mf_roundtrip_mediacapture(nv12_tex_t nv12_tex)
	{
		_nv12_tex = nv12_tex;
		try
		{
			UINT32 width = 1920, height = 1080, fps = 24;
			UINT32 bitrate = 5000000;
			
			ComPtr<IMFMediaType> pInputMediaType;
			ThrowIfFailed(MFCreateMediaType(pInputMediaType.GetAddressOf()));
			mf_set_default_media_type(pInputMediaType.Get(), MFVideoFormat_NV12, bitrate, width, height, fps);

			ComPtr<IMFMediaType> pOutputMediaType;
			ThrowIfFailed(MFCreateMediaType(pOutputMediaType.GetAddressOf()));
			mf_set_default_media_type(pOutputMediaType.Get(), MFVideoFormat_H264, bitrate, width, height, fps);

			mf_create_mft_software_encoder(pInputMediaType.Get(), pOutputMediaType.Get(), pDecoderTransform.GetAddressOf(), &ppActivate);
			
			// Apply H264 settings and update the output media type
			ThrowIfFailed(pOutputMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base));
			ThrowIfFailed(pDecoderTransform->SetOutputType(0, pOutputMediaType.Get(), 0));
		}
		catch (const std::exception&)
		{
			log_err("Fatal! Failed to decode from url.");
			throw;
		}
	}

	static void mf_roundtrip_mediacapture_shutdown()
	{
		pEncoderTransform.Reset();
		pDecoderTransform.Reset();

		if (ppActivate && *ppActivate)
		{
			CoTaskMemFree(ppActivate);
		}
	}
} // namespace nakamir