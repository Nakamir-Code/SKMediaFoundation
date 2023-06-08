#include <stereokit.h>
#include <stereokit_ui.h>
#include "../nv12_tex.h"
#include "../nv12_sprite.h"
#include "../mf_encoder.h"
#include "../mf_decoder.h"
#include "../mf_utility.h"
#include "../error.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <atomic>
#include <thread>

#pragma comment(lib, "Mf.lib")

using Microsoft::WRL::ComPtr;

using namespace sk;

namespace nakamir {

	const UINT32 bitrate = 5000000;

	// PRIVATE METHODS
	static void mf_roundtrip_webcam_impl(UINT32* width, UINT32* height);
	static void mf_source_reader_roundtrip(const ComPtr<IMFSourceReader>& pSourceReader, const ComPtr<IMFTransform>& pEncoderTransform, const ComPtr<IMFTransform>& pDecoderTransform);
	static void mf_shutdown_thread();

	static IMFActivate** ppEncoderActivate = NULL;
	static IMFActivate** ppDecoderActivate = NULL;
	static IMFActivate** ppVideoActivate = NULL;
	static ComPtr<IMFMediaSource> pVideoSource;
	static ComPtr<IMFSourceReader> pSourceReader;
	static ComPtr<IMFTransform> pEncoderTransform;
	static ComPtr<IMFTransform> pDecoderTransform;
	static std::thread sourceReaderThread;
	static std::atomic_bool _cancellationToken;

	static pose_t window_pose = { {0,0.25f,-0.3f}, quat_from_angles(20,-180,0) };

	const float video_plane_width = 0.6f;
	const vec2 video_window_padding = { 0.02f, 0.02f };
	static vec2 video_aspect_ratio;
	static matrix video_render_matrix;

	static nv12_tex_t nv12_tex;
	static nv12_sprite_t nv12_sprite;

	void mf_roundtrip_webcam() {
		sk_settings_t settings = {};
		settings.app_name = "SKVideoDecoder";
		settings.assets_folder = "Assets";
		settings.display_preference = display_mode_mixedreality;
		if (!sk_init(settings))
			return;

		if (FAILED(MFStartup(MF_VERSION)))
			return;

		// Read from the webcam, encode the sample, decode the sample, and finally render to the screen
		UINT32 video_width;
		UINT32 video_height;
		mf_roundtrip_webcam_impl(&video_width, &video_height);

		// Set up the render plane based on the video dimensions
		video_aspect_ratio = { video_plane_width, video_height / (float)video_width * video_plane_width };
		video_render_matrix = matrix_ts({ 0, -video_aspect_ratio.y / 2, -.002f }, { (video_aspect_ratio.x - video_window_padding.x), (video_aspect_ratio.y - video_window_padding.y), 0 });

		nv12_tex = nv12_tex_create(video_width, video_height);
		nv12_sprite = nv12_sprite_create(nv12_tex, sprite_type_atlased);

		// Run the source reader on a separate thread
		sourceReaderThread = std::thread(mf_source_reader_roundtrip, pSourceReader, pEncoderTransform, pDecoderTransform);

		sk_run(
			[]() {
				ui_window_begin("Video", window_pose, video_aspect_ratio, ui_win_normal, ui_move_face_user);
				ui_nextline();
				ui_text("  Stats for Nerds");
				ui_text("  w: , h: , fps: ");
				ui_text("  Encoder Avg time: ");
				ui_text("  Decoder Avg time: ");

				nv12_sprite_ui_image(nv12_sprite, video_render_matrix);
				ui_window_end();
			},
			[]() {
				mf_shutdown_thread();
			});

		nv12_tex_release(nv12_tex);
		nv12_sprite_release(nv12_sprite);

		if (FAILED(MFShutdown())) {
			log_err("MFShutdown call failed!");
			return;
		}
	}

	static void mf_roundtrip_webcam_impl(UINT32* width, UINT32* height)
	{
		try
		{
			// Get the first available webcam.
			ComPtr<IMFAttributes> pVideoConfig;
			ThrowIfFailed(MFCreateAttributes(pVideoConfig.GetAddressOf(), 1));
			ThrowIfFailed(pVideoConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

			UINT32 videoDeviceCount = 0;
			ThrowIfFailed(MFEnumDeviceSources(pVideoConfig.Get(), &ppVideoActivate, &videoDeviceCount));

			if (videoDeviceCount == 0) {
				throw std::exception("No webcams found!");
			}

			WCHAR* friendlyName;
			UINT32 nameLength;
			ThrowIfFailed(ppVideoActivate[0]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLength));
			wprintf(L"Using webcam: %s\n", friendlyName);

			ThrowIfFailed(ppVideoActivate[0]->ActivateObject(IID_PPV_ARGS(pVideoSource.GetAddressOf())));

			// Create attributes for the source reader
			ComPtr<IMFAttributes> pSourceReaderAttributes;
			ThrowIfFailed(MFCreateAttributes(pSourceReaderAttributes.GetAddressOf(), 0));

			// Create a source reader from the media source
			ThrowIfFailed(MFCreateSourceReaderFromMediaSource(pVideoSource.Get(), pSourceReaderAttributes.Get(), pSourceReader.GetAddressOf()));

			// Get the current media type of the first video stream
			ComPtr<IMFMediaType> pInputMediaType;
			ThrowIfFailed(pSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, pInputMediaType.GetAddressOf()));

			UINT32 fps, den;
			ThrowIfFailed(MFGetAttributeSize(pInputMediaType.Get(), MF_MT_FRAME_SIZE, width, height));
			ThrowIfFailed(MFGetAttributeRatio(pInputMediaType.Get(), MF_MT_FRAME_RATE, &fps, &den));
			mf_set_default_media_type(pInputMediaType.Get(), MFVideoFormat_NV12, bitrate, *width, *height, fps);

			ComPtr<IMFMediaType> pOutputMediaType;
			ThrowIfFailed(MFCreateMediaType(pOutputMediaType.GetAddressOf()));
			mf_set_default_media_type(pOutputMediaType.Get(), MFVideoFormat_H264, bitrate, *width, *height, fps);

			// Create encoder
			mf_create_mft_software_encoder(pInputMediaType.Get(), pOutputMediaType.Get(), pEncoderTransform.GetAddressOf(), &ppEncoderActivate);
			// Create decoder
			_VIDEO_DECODER decoderType = mf_create_mft_software_decoder(pOutputMediaType.Get(), pInputMediaType.Get(), pDecoderTransform.GetAddressOf(), &ppDecoderActivate);

			// Apply H264 settings and update the output media type
			ThrowIfFailed(pInputMediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base));
			ThrowIfFailed(pDecoderTransform->SetOutputType(0, pInputMediaType.Get(), 0));
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw;
		}
	}

	static void mf_source_reader_roundtrip(const ComPtr<IMFSourceReader>& pSourceReader, const ComPtr<IMFTransform>& pEncoderTransform, const ComPtr<IMFTransform>& pDecoderTransform)
	{
		try
		{
			// Send messages to encoder to start streaming
			ThrowIfFailed(pEncoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
			ThrowIfFailed(pEncoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

			// Send messages to decoder to flush data and start streaming
			ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
			ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
			ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

			// Start processing frames
			LONGLONG llSampleTime = 0, llSampleDuration = 0;
			while (!_cancellationToken)
			{
				ComPtr<IMFSample> pVideoSample;
				DWORD streamIndex, flags;
				ThrowIfFailed(pSourceReader->ReadSample(
					MF_SOURCE_READER_FIRST_VIDEO_STREAM,
					0,                              // Flags.
					&streamIndex,                   // Receives the actual stream index. 
					&flags,                         // Receives status flags.
					&llSampleTime,					// Receives the timestamp.
					pVideoSample.GetAddressOf()     // Receives the sample or NULL.
				));

				if (flags & MF_SOURCE_READERF_STREAMTICK)
				{
					printf("\tStream tick.\n");
				}
				if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
				{
					printf("\tEnd of stream.\n");
					break;
				}

				if (pVideoSample)
				{
					ThrowIfFailed(pVideoSample->SetSampleTime(llSampleTime));
					ThrowIfFailed(pVideoSample->GetSampleDuration(&llSampleDuration));

					// Encode the sample
					mf_encode_sample_to_buffer(pVideoSample.Get(), pEncoderTransform.Get(),
						[&pDecoderTransform](IMFSample* pEncodedSample) {
							// Decode the sample
							mf_decode_sample_to_buffer(pEncodedSample, pDecoderTransform.Get(),
								[](IMFSample* pDecodedSample) {
									// Write the decoded sample to the nv12 texture
									ComPtr<IMFMediaBuffer> buffer;
									ThrowIfFailed(pDecodedSample->GetBufferByIndex(0, buffer.GetAddressOf()));

									DWORD bufferLength;
									ThrowIfFailed(buffer->GetCurrentLength(&bufferLength));

									byte* byteBuffer = NULL;
									DWORD maxLength = 0, currentLength = 0;
									ThrowIfFailed(buffer->Lock(&byteBuffer, &maxLength, &currentLength));
									nv12_tex_set_buffer(nv12_tex, byteBuffer);
									ThrowIfFailed(buffer->Unlock());
								});
						}
					);
				}
			}
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw;
		}
	}

	static void mf_shutdown_thread()
	{
		_cancellationToken = true;
		sourceReaderThread.join();

		pSourceReader.Reset();
		pDecoderTransform.Reset();

		if (ppEncoderActivate && *ppEncoderActivate)
		{
			CoTaskMemFree(ppEncoderActivate);
		}
		if (ppDecoderActivate && *ppDecoderActivate)
		{
			CoTaskMemFree(ppDecoderActivate);
		}
		if (ppVideoActivate && *ppVideoActivate)
		{
			CoTaskMemFree(ppVideoActivate);
		}
	}
} // namespace nakamir