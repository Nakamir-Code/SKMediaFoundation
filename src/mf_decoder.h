#pragma once

#include <mfapi.h>
#include <mftransform.h>
#include <functional>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace nakamir {

	enum _VIDEO_DECODER
	{
		SOFTWARE_MFT_VIDEO_DECODER,
		D3D11_MFT_VIDEO_DECODER,
	};

	_VIDEO_DECODER mf_create_mft_software_decoder(/**[in]**/ const GUID& targetSubType, /**[in,out]**/ ComPtr<IMFMediaType>& pInputMediaType, /**[out]**/ ComPtr<IMFMediaType>& pOutputMediaType, /**[out]**/ ComPtr<IMFTransform>& pDecoderTransform, /**[out]**/ IMFActivate*** pppActivate);
	void mf_decode_sample_to_buffer(/**[in]**/ const ComPtr<IMFSample> pVideoSample, /**[in]**/ const ComPtr<IMFTransform> pDecoderTransform, /**[in]**/ const std::function<void(byte*)>& onReceiveImageBuffer);

} // namespace nakamir