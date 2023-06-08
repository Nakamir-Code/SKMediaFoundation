#pragma once

#include "nv12_tex.h"
#include <functional>

namespace nakamir {
	void mf_decode_from_url(const wchar_t* filename, nv12_tex_t nv12_tex, std::function<void()>* shutdown_thread);
	void mf_roundtrip_mediacapture(nv12_tex_t nv12_tex);
} // namespace nakamir