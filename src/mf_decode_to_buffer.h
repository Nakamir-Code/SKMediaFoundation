#pragma once

#include "nv12_tex.h"

namespace nakamir {
	bool32_t mf_initialize(nv12_tex_t nv12_tex);
	void mf_read_from_url(const wchar_t* filename);
	void mf_roundtrip_from_webcam();
} // namespace nakamir