#pragma once

#include <mfapi.h>
#include <mftransform.h>
#include <functional>

namespace nakamir {

	enum _VIDEO_DECODER
	{
		SOFTWARE_MFT_VIDEO_DECODER,
		D3D11_MFT_VIDEO_DECODER,
	};

	_VIDEO_DECODER mf_create_mft_software_decoder(/**[in]**/ IMFMediaType* pInputMediaType, /**[in]**/ IMFMediaType* pOutputMediaType, /**[out]**/ IMFTransform** ppDecoderTransform, /**[out]**/ IMFActivate*** pppActivate);
	void mf_decode_sample_to_buffer(/**[in]**/ IMFSample* pVideoSample, /**[in]**/ IMFTransform* pDecoderTransform, /**[in]**/ const std::function<void(byte*)>& onReceiveImageBuffer);

} // namespace nakamir