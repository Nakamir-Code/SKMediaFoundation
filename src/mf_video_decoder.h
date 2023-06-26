#pragma once

#include "mf_utility.h"
#include <mftransform.h>

namespace nakamir {
	_MFT_TYPE mf_create_mft_video_decoder(/**[in]**/ IMFMediaType* pInputMediaType, /**[in]**/ IMFMediaType* pOutputMediaType, /**[out]**/ IMFTransform** ppDecoderTransform, /**[out]**/ IMFActivate*** pppActivate);
} // namespace nakamir