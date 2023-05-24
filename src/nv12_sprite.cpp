#include "nv12_sprite.h"
#include "sk_memory.h"

namespace nakamir {

	nv12_sprite_t nv12_sprite_create(nv12_tex_t nv12_tex, sprite_type_ sprite_type, const char* atlas_id) {
		nv12_sprite_t nv12_sprite = (nv12_sprite_t)sk_malloc(sizeof(_nv12_sprite_t));
		nv12_sprite->nv12_tex = nv12_tex;
		nv12_sprite->current_material = nv12_tex->material;
		nv12_sprite->nv12_overlap_material = nullptr;
		return nv12_sprite;
	}

	void nv12_sprite_release(nv12_sprite_t nv12_sprite) {
		material_release(nv12_sprite->nv12_overlap_material);
		sk_free(nv12_sprite);
	}

	void nv12_sprite_ui_image(nv12_sprite_t nv12_sprite, matrix render_matrix) {
		mesh_t mesh_quad = mesh_find(default_id_mesh_quad);
		mesh_draw(mesh_quad, nv12_sprite->current_material, render_matrix);
		mesh_release(mesh_quad);
	}

} // namespace nakamir
