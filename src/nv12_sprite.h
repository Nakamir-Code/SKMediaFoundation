#pragma once

#include <stereokit.h>
#include "nv12_tex.h"

using namespace sk;

namespace nakamir {

	SK_DeclarePrivateType(nv12_sprite_t);

	struct _nv12_sprite_t {
		nv12_tex_t nv12_tex;
		material_t nv12_overlap_material;
		material_t current_material;
	};

	nv12_sprite_t nv12_sprite_create(nv12_tex_t nv12_tex, sprite_type_ sprite_type, const char* atlas_id = "default");
	void nv12_sprite_release(nv12_sprite_t nv12_sprite);
	void nv12_sprite_ui_image(nv12_sprite_t nv12_sprite, matrix render_matrix);

} // namespace nakamir
