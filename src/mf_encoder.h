#pragma once

#include <mfapi.h>
#include <mftransform.h>
#include <functional>

namespace nakamir {
	void mf_create_mft_software_encoder(/**[in]**/ IMFMediaType* pInputMediaType, /**[in]**/ IMFMediaType* pOutputMediaType, /**[out]**/ IMFTransform** ppEncoderTransform, /**[out]**/ IMFActivate*** pppActivate);
} // namespace nakamir