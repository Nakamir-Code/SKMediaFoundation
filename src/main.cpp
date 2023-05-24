#include <stereokit.h>
#include <stereokit_ui.h>
#include "nv12_tex.h"
#include "nv12_sprite.h"
using namespace sk;
using namespace nakamir;

pose_t window_pose = {{0,0.25f,-0.3f}, quat_from_angles(20,-180,0)};

int video_width = 1920;
int video_height = 1080;

float video_plane_width = 0.6;
vec2 video_aspect_ratio = {video_plane_width, video_height / (float)video_width * video_plane_width};
vec2 video_window_padding = {0.02, 0.02};
matrix video_render_matrix = matrix_ts({0, -video_aspect_ratio.y / 2, -.002}, {(video_aspect_ratio.x - video_window_padding.x), (video_aspect_ratio.y - video_window_padding.y), 0});

nv12_tex_t nv12_tex;
nv12_sprite_t nv12_sprite;

int main(void) {
    sk_settings_t settings = {};
	settings.app_name           = "SKVideoDecoder";
	settings.assets_folder      = "Assets";
	settings.display_preference = display_mode_mixedreality;
	if (!sk_init(settings))
		return 1;

    nv12_tex = nv12_tex_create(video_width, video_height);
    nv12_sprite = nv12_sprite_create(nv12_tex, sprite_type_atlased);

    sk_run([]() {
        ui_window_begin("Video", window_pose, video_aspect_ratio, ui_win_normal, ui_move_face_user);
        nv12_sprite_ui_image(nv12_sprite, video_render_matrix);
        ui_window_end();
	});

    clean();
    return 0;
}

void clean() {
    nv12_tex_release(nv12_tex);
    nv12_sprite_release(nv12_sprite);
}