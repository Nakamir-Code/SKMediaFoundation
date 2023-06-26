#pragma once

#include "mf_utility.h"

namespace nakamir {
	_MFT_TYPE mf_create_mft_video_encoder(/**[in]**/ IMFMediaType* pInputMediaType, /**[in]**/ IMFMediaType* pOutputMediaType, /**[out]**/ IMFTransform** ppEncoderTransform, /**[out]**/ IMFActivate*** pppActivate);
} // namespace nakamir