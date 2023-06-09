#pragma once

namespace nakamir {
	void mf_decode_from_url(/**[in]**/ const wchar_t* filename);
#ifndef WINDOWS_UWP
	void mf_roundtrip_webcam();
#endif
} // namespace nakamir