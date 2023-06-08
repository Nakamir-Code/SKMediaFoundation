#pragma once

#include <mfapi.h>
#include <mftransform.h>
#include <functional>

namespace nakamir {

	void mf_create_mft_software_encoder(/**[in]**/ IMFMediaType* pInputMediaType, /**[in]**/ IMFMediaType* pOutputMediaType, /**[out]**/ IMFTransform** ppEncoderTransform, /**[out]**/ IMFActivate*** pppActivate);
	void mf_encode_sample_to_buffer(/**[in]**/ IMFSample* pVideoSample, /**[in]**/ IMFTransform* pEncoderTransform, /**[in]**/ const std::function<void(uint32_t, byte*)>& onReceiveEncodedBuffer);

} // namespace nakamir