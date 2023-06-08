#include "nv12_tex.h"
#include "sk_memory.h"
#include "error.h"

namespace nakamir {

	nv12_tex_t nv12_tex_create(int width, int height) {
		// TODO: this shader must be included as builtin in Nakamir's assets.
		shader_t nv12_quad_shader = shader_create_file("nv12_quad.hlsl");
		if (nv12_quad_shader == nullptr) {
			log_err("NV12 quad shader not found!");
			return nullptr;
		}
		material_t material = material_create(nv12_quad_shader);
		shader_release(nv12_quad_shader);

		tex_t luminance_tex = tex_create(tex_type_image_nomips | tex_type_dynamic, tex_format_r8);
		tex_t chrominance_tex = tex_create(tex_type_image_nomips | tex_type_dynamic, tex_format_r8g8);

		uint8_t* luminance_data = sk_malloc_t(uint8_t, width * height);
		tex_set_colors(luminance_tex, width, height, luminance_data);
		sk_free(luminance_data);

		uint16_t* chrominance_data = sk_malloc_t(uint16_t, (width / 2) * (height / 2));
		tex_set_colors(chrominance_tex, width / 2, height / 2, chrominance_data);
		sk_free(chrominance_data);

		material_set_texture(material, "luminance", luminance_tex);
		material_set_texture(material, "chrominance", chrominance_tex);

		nv12_tex_t nv12_tex = (nv12_tex_t)sk_malloc(sizeof(_nv12_tex_t));
		nv12_tex->width = width;
		nv12_tex->height = height;
		nv12_tex->material = material;
		nv12_tex->luminance_tex = luminance_tex;
		nv12_tex->luminance_view = (ID3D11Texture2D*)tex_get_surface(luminance_tex);
		nv12_tex->chrominance_tex = chrominance_tex;
		nv12_tex->chrominance_view = (ID3D11Texture2D*)tex_get_surface(chrominance_tex);
		return nv12_tex;
	}

	void nv12_tex_release(nv12_tex_t nv12_tex) {
		tex_release(nv12_tex->luminance_tex);
		tex_release(nv12_tex->chrominance_tex);
		material_release(nv12_tex->material);
		sk_free(nv12_tex);
	}

	void nv12_tex_set_buffer(nv12_tex_t nv12_tex, const unsigned char* encoded_image_buffer, int offset) {

		// add the offset if there is one
		encoded_image_buffer += offset;

		ID3D11Device* pD3D_device = (ID3D11Device*)backend_d3d11_get_d3d_device();

		// For dynamic textures, just upload the new value into the texture!
		D3D11_MAPPED_SUBRESOURCE tex_mem = {};

		bool on_main = backend_d3d11_get_main_thread_id() == GetCurrentThreadId();
		ID3D11DeviceContext* pContext;
		if (on_main) {
			pD3D_device->GetImmediateContext(&pContext);
		}
		else {
			pContext = (ID3D11DeviceContext*)backend_d3d11_get_deferred_d3d_context();
			WaitForSingleObject(backend_d3d11_get_deferred_mtx(), INFINITE);
		}

		try
		{
			int luminance_size = nv12_tex->width * nv12_tex->height;
			ThrowIfFailed(pContext->Map(nv12_tex->luminance_view, 0, D3D11_MAP_WRITE_DISCARD, 0, &tex_mem));
			memcpy(tex_mem.pData, encoded_image_buffer, (size_t)luminance_size);
			pContext->Unmap(nv12_tex->luminance_view, 0);

			encoded_image_buffer += luminance_size;

			int chrominance_size = (nv12_tex->width / 2) * (nv12_tex->height / 2) * sizeof(uint16_t);
			ThrowIfFailed(pContext->Map(nv12_tex->chrominance_view, 0, D3D11_MAP_WRITE_DISCARD, 0, &tex_mem));
			memcpy(tex_mem.pData, encoded_image_buffer, (size_t)chrominance_size);
			pContext->Unmap(nv12_tex->chrominance_view, 0);
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
		}

		if (!on_main) {
			ReleaseMutex(backend_d3d11_get_deferred_mtx());
		}
	}
} // namespace nakamir
