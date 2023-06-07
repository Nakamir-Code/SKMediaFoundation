#pragma once

#include <mfapi.h>
#include <mftransform.h>
#include <functional>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace nakamir {

	void mf_create_mft_software_encoder(/**[in]**/ const GUID& targetSubType, /**[in,out]**/ ComPtr<IMFMediaType>& pInputMediaType, /**[out]**/ ComPtr<IMFMediaType>& pOutputMediaType, /**[out]**/ ComPtr<IMFTransform>& pEncoderTransform, /**[out]**/ IMFActivate*** pppActivate);
	//void mf_encode_sample_to_buffer(IMFSample* pVideoSample, IMFTransform* pEncoderTransform, std::function<void(byte*)> onReceiveEncodedBuffer);

} // namespace nakamir