#pragma once

#include <stereokit.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d11.h>

using namespace sk;

namespace nakamir {

	SK_DeclarePrivateType(nv12_tex_t);

	struct _nv12_tex_t {
		int width;
		int height;
		material_t material;
		tex_t luminance_tex;
		tex_t chrominance_tex;
		ID3D11Texture2D* luminance_view;
		ID3D11Texture2D* chrominance_view;
	};

	nv12_tex_t nv12_tex_create(int width, int height);
	void nv12_tex_release(nv12_tex_t nv12_tex);
	void nv12_tex_set_buffer_cpu(nv12_tex_t nv12_tex, const unsigned char* encoded_image_buffer, int offset = 0);
	void nv12_tex_set_buffer_gpu(nv12_tex_t nv12_tex, ID3D11Texture2D* d3d_texture);

} // namespace nakamir
