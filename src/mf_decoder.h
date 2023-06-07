#pragma once

#include "nv12_tex.h"
#include <mfapi.h>
#include <mftransform.h>

namespace nakamir {

	enum _VIDEO_DECODER
	{
		D3D_VIDEO_DECODER,
		SOFTWARE_MFT_VIDEO_DECODER,
	};

	_VIDEO_DECODER create_video_decoder(IMFMediaType* pInputMediaType, IMFTransform* pDecoderTransform,  /**[out]**/ IMFDXGIDeviceManager** ppDXGIDeviceManager);

	void mf_create_mft_software_decoder(IMFMediaType* pInputMediaType, /*[out]*/ IMFTransform** ppDecoderTransform, /*[out]*/ IMFActivate*** pppActivate);

	void mf_initialize_mft_software_decoder(IMFMediaType* pInputMediaType, IMFMediaType* pOutputMediaType, IMFTransform* pDecoderTransform);

	//void mf_initialize_d3d_video_decoder(IMFMediaType* pInputMediaType, IMFTransform* pDecoderTransform, IMFDXGIDeviceManager* pDXGIDeviceManager);

	void mf_decode_sample_cpu(IMFSample* pVideoSample, IMFTransform* pDecoderTransform, nv12_tex_t nv12_tex);

	void mf_print_stream_info(IMFTransform* pDecoderTransform);
} // namespace nakamir