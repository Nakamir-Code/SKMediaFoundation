#include <stereokit.h>
#include <stereokit_ui.h>
#include "nv12_sprite.h"
#include "mf_decoder.h"
#include "mf_examples.h"
#include <mfapi.h>

using namespace sk;
using namespace nakamir;

pose_t window_pose = { {0,0.25f,-0.3f}, quat_from_angles(20,-180,0) };

int video_width = 1280;
int video_height = 720;

float video_plane_width = 0.6f;
vec2 video_aspect_ratio = { video_plane_width, video_height / (float)video_width * video_plane_width };
vec2 video_window_padding = { 0.02f, 0.02f };
matrix video_render_matrix = matrix_ts({ 0, -video_aspect_ratio.y / 2, -.002f }, { (video_aspect_ratio.x - video_window_padding.x), (video_aspect_ratio.y - video_window_padding.y), 0 });

nv12_tex_t nv12_tex;
nv12_sprite_t nv12_sprite;
std::function<void()> mf_shutdown_thread = nullptr;

int main(void) {
	sk_settings_t settings = {};
	settings.app_name = "SKVideoDecoder";
	settings.assets_folder = "Assets";
	settings.display_preference = display_mode_mixedreality;
	if (!sk_init(settings))
		return 1;

	nv12_tex = nv12_tex_create(video_width, video_height);
	nv12_sprite = nv12_sprite_create(nv12_tex, sprite_type_atlased);

	if (FAILED(MFStartup(MF_VERSION)))
		return 1;

	// SCENARIO 1: Decode an MP4 file from a local or online source as fast as possible.
	mf_decode_from_url(L"http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4", nv12_tex, &mf_shutdown_thread);

	// SCENARIO 2: 
	mf_roundtrip_mediacapture(nv12_tex);

	sk_run(
		[]() {
			ui_window_begin("Video", window_pose, video_aspect_ratio, ui_win_normal, ui_move_face_user);
			nv12_sprite_ui_image(nv12_sprite, video_render_matrix);
			ui_window_end();
		},
		[]() {
			if (mf_shutdown_thread)
				mf_shutdown_thread();
		});

	nv12_tex_release(nv12_tex);
	nv12_sprite_release(nv12_sprite);

	if (FAILED(MFShutdown())) {
		log_err("MFShutdown call failed!");
		return 1;
	}

	return 0;
}