#pragma once

#include <mftransform.h>

namespace nakamir {
	void mf_create_mft_video_encoder(/**[in]**/ IMFMediaType* pInputMediaType, /**[in]**/ IMFMediaType* pOutputMediaType, /**[out]**/ IMFTransform** ppEncoderTransform, /**[out]**/ IMFActivate*** pppActivate);
} // namespace nakamir