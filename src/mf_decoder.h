#pragma once

#include <mftransform.h>

namespace nakamir {
	enum _VIDEO_DECODER
	{
		SOFTWARE_MFT_VIDEO_DECODER,
		D3D11_MFT_VIDEO_DECODER,
	};
	
	_VIDEO_DECODER mf_create_mft_video_decoder(/**[in]**/ IMFMediaType* pInputMediaType, /**[in]**/ IMFMediaType* pOutputMediaType, /**[out]**/ IMFTransform** ppDecoderTransform, /**[out]**/ IMFActivate*** pppActivate);
} // namespace nakamir