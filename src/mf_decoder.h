#pragma once

#include "nv12_tex.h"
#include <mfapi.h>
#include <mftransform.h>

namespace nakamir {

	void mf_decode_from_url(const wchar_t* filename, nv12_tex_t nv12_tex);

	static void mf_create_mft_software_decoder(IMFMediaType* pInputMediaType, /*[out]*/ IMFTransform** pDecoderTransform);

	static void mf_initialize_mft_software_decoder(IMFMediaType* pInputMediaType, IMFMediaType* pOutputMediaType, IMFTransform* pDecoderTransform);
	
	static void mf_decode_sample_cpu(IMFSample* pVideoSample, IMFTransform* pDecoderTransform);

	static void mf_print_stream_info(IMFTransform* pDecoderTransform);
} // namespace nakamir