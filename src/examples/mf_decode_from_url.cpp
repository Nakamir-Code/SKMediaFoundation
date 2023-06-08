#include <stereokit.h>
#include <stereokit_ui.h>
#include "../nv12_tex.h"
#include "../nv12_sprite.h"
#include "../mf_decoder.h"
#include "../error.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <atomic>
#include <thread>

using Microsoft::WRL::ComPtr;

using namespace sk;

namespace nakamir {
	
	// PRIVATE METHODS
	static void mf_decode_from_url_impl(const wchar_t* filename, UINT32* width, UINT32* height);
	static void mf_decode_source_reader_to_buffer(const ComPtr<IMFSourceReader>& pSourceReader, const ComPtr<IMFTransform>& pDecoderTransform);
	static void mf_shutdown_thread();

	static IMFActivate** ppActivate = NULL;
	static ComPtr<IMFSourceReader> pSourceReader;
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

	void mf_decode_from_url(const wchar_t* filename) {
		sk_settings_t settings = {};
		settings.app_name = "SKVideoDecoder";
		settings.assets_folder = "Assets";
		settings.display_preference = display_mode_mixedreality;
		if (!sk_init(settings))
			return;

		if (FAILED(MFStartup(MF_VERSION)))
			return;

		// Decode an MP4 file from a local or online source as fast as possible.
		UINT32 video_width;
		UINT32 video_height;
		mf_decode_from_url_impl(L"http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4", &video_width, &video_height);

		// Set up the render plane based on the video dimensions
		video_aspect_ratio = { video_plane_width, video_height / (float)video_width * video_plane_width };
		video_render_matrix = matrix_ts({ 0, -video_aspect_ratio.y / 2, -.002f }, { (video_aspect_ratio.x - video_window_padding.x), (video_aspect_ratio.y - video_window_padding.y), 0 });

		nv12_tex = nv12_tex_create(video_width, video_height);
		nv12_sprite = nv12_sprite_create(nv12_tex, sprite_type_atlased);

		// Run the source reader on a separate thread
		sourceReaderThread = std::thread(mf_decode_source_reader_to_buffer, pSourceReader, pDecoderTransform);

		sk_run(
			[]() {
				ui_window_begin("Video", window_pose, video_aspect_ratio, ui_win_normal, ui_move_face_user);
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

	static void mf_decode_from_url_impl(const wchar_t* filename, UINT32* width, UINT32* height)
	{
		_cancellationToken = false;
		try
		{
			// Set up the reader for the file
			ComPtr<IMFSourceResolver> pSourceResolver;
			ThrowIfFailed(MFCreateSourceResolver(&pSourceResolver));

			// Create a media source object from a URL
			MF_OBJECT_TYPE objectType = MF_OBJECT_INVALID;
			ComPtr<IUnknown> uSource;
			ThrowIfFailed(pSourceResolver->CreateObjectFromURL(
				filename,					// URL of the source.
				MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ,  // Create a source object.
				NULL,                       // Optional property store.
				&objectType,				// Receives the created object type. 
				&uSource					// Receives a pointer to the media source.
			));

			// Get a pointer to the media source
			ComPtr<IMFMediaSource> mediaFileSource;
			ThrowIfFailed(uSource->QueryInterface(IID_PPV_ARGS(mediaFileSource.GetAddressOf())));

			// Create attributes for the source reader
			ComPtr<IMFAttributes> pVideoReaderAttributes;
			ThrowIfFailed(MFCreateAttributes(pVideoReaderAttributes.GetAddressOf(), 0));

			// Create a source reader from the media source
			ThrowIfFailed(MFCreateSourceReaderFromMediaSource(mediaFileSource.Get(), pVideoReaderAttributes.Get(), pSourceReader.GetAddressOf()));

			// Get the current media type of the first video stream
			ComPtr<IMFMediaType> pInputMediaType;
			ThrowIfFailed(pSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, pInputMediaType.GetAddressOf()));

			ComPtr<IMFMediaType> pOutputMediaType;
			ThrowIfFailed(MFCreateMediaType(pOutputMediaType.GetAddressOf()));
			ThrowIfFailed(pInputMediaType->CopyAllItems(pOutputMediaType.Get()));
			ThrowIfFailed(pOutputMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));

			// Get the width and height of the output video
			ThrowIfFailed(MFGetAttributeSize(pOutputMediaType.Get(), MF_MT_FRAME_SIZE, width, height));

			_VIDEO_DECODER decoderType = mf_create_mft_software_decoder(pInputMediaType.Get(), pOutputMediaType.Get(), pDecoderTransform.GetAddressOf(), &ppActivate);

			switch (decoderType)
			{
			case SOFTWARE_MFT_VIDEO_DECODER:
			{
				break;
			}
			case D3D11_MFT_VIDEO_DECODER:
			{
				ComPtr<IMFAttributes> pAttributes;
				ThrowIfFailed(pDecoderTransform->GetAttributes(pAttributes.GetAddressOf()));
				ThrowIfFailed(pAttributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE));
				break;
			}
			default: throw std::exception("Decoder type not found!");
			}
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw;
		}
	}

	static void mf_decode_source_reader_to_buffer(const ComPtr<IMFSourceReader>& pSourceReader, const ComPtr<IMFTransform>& pDecoderTransform)
	{
		try
		{
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

					mf_decode_sample_to_buffer(pVideoSample.Get(), pDecoderTransform.Get(),
						[](IMFSample* pDecodedSample) {
							// Write the decoded sample to the nv12 texture
							ComPtr<IMFMediaBuffer> buffer;
							ThrowIfFailed(pDecodedSample->GetBufferByIndex(0, buffer.GetAddressOf()));

							DWORD bufferLength;
							ThrowIfFailed(buffer->GetCurrentLength(&bufferLength));

							//printf("Sample size %i.\n", bufferLength);

							byte* byteBuffer = NULL;
							DWORD maxLength = 0, currentLength = 0;
							ThrowIfFailed(buffer->Lock(&byteBuffer, &maxLength, &currentLength));
							nv12_tex_set_buffer(nv12_tex, byteBuffer);
							ThrowIfFailed(buffer->Unlock());
						});
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

		if (ppActivate && *ppActivate)
		{
			CoTaskMemFree(ppActivate);
		}
	}
} // namespace nakamir