#ifndef WINDOWS_UWP
#include <stereokit.h>
#include <stereokit_ui.h>
#include "../nv12_tex.h"
#include "../nv12_sprite.h"
#include "../mf_video_encoder.h"
#include "../mf_video_decoder.h"
#include "../mf_utility.h"
#include "../error.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <atomic>
#include <format>
#include <thread>

// Settings
#define PRINT_MBPS 1

using Microsoft::WRL::ComPtr;
using namespace sk;

namespace nakamir {

	const UINT32 bitrate = 3000000;

	// PRIVATE METHODS
	static void mf_roundtrip_webcam_impl(/**[out]**/ UINT32* width, /**[out]**/ UINT32* height, /**[out]**/ UINT32* fps);
	static void mf_source_reader_roundtrip(/**[in]**/ const ComPtr<IMFSourceReader>& pSourceReader, /**[in]**/ const ComPtr<IMFTransform>& pEncoderTransform, /**[in]**/ const ComPtr<IMFTransform>& pDecoderTransform);
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

	static UINT32 video_width;
	static UINT32 video_height;
	static UINT32 video_fps;
	static vec2 video_aspect_ratio;
	static matrix video_render_matrix;

	static nv12_tex_t nv12_tex;
	static nv12_sprite_t nv12_sprite;

#if PRINT_MBPS
	static UINT64 _avg_byte_size = 0;
	static UINT64 _num_frames = 0;
#endif

	void mf_roundtrip_webcam() {
		sk_settings_t settings = {};
		settings.app_name = "MF Roundtrip Webcam";
		settings.assets_folder = "Assets";
		settings.display_preference = display_mode_mixedreality;
		if (!sk_init(settings))
			return;

		if (FAILED(MFStartup(MF_VERSION)))
			return;

		mf_roundtrip_webcam_impl(&video_width, &video_height, &video_fps);

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
				ui_text(std::format("\t{}x{} @ {} fps", video_width, video_height, video_fps).c_str());
				nv12_sprite_ui_image(nv12_sprite, video_render_matrix);
				ui_window_end();
			}, mf_shutdown_thread);

		nv12_tex_release(nv12_tex);
		nv12_sprite_release(nv12_sprite);

		if (FAILED(MFShutdown())) {
			log_err("MFShutdown call failed!");
			return;
		}
	}

	static void mf_roundtrip_webcam_impl(UINT32* width, UINT32* height, UINT32* fps)
	{
		try
		{
			// Get the first available webcam
			ComPtr<IMFAttributes> pVideoConfig;
			ThrowIfFailed(MFCreateAttributes(pVideoConfig.GetAddressOf(), 2));
			ThrowIfFailed(pVideoConfig->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
			ThrowIfFailed(pVideoConfig->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

			UINT32 videoDeviceCount = 0;
			ThrowIfFailed(MFEnumDeviceSources(pVideoConfig.Get(), &ppVideoActivate, &videoDeviceCount));

			if (videoDeviceCount == 0) {
				throw std::exception("No webcams found!");
			}

			WCHAR* friendlyName = nullptr;
			UINT32 nameLength;
			ThrowIfFailed(ppVideoActivate[0]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLength));
			if (!friendlyName) throw std::exception("Could not get the friendly name of the webcam!");
			// Log the webcam name
			int requiredSize = WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, nullptr, 0, nullptr, nullptr);
			char* friendlyNameBuffer = new char[requiredSize];
			WideCharToMultiByte(CP_UTF8, 0, friendlyName, -1, friendlyNameBuffer, requiredSize, nullptr, nullptr);
			std::string result = "Using webcam: ";
			result += friendlyNameBuffer;
			log_info(result.c_str());
			delete[] friendlyNameBuffer;

			ThrowIfFailed(ppVideoActivate[0]->ActivateObject(IID_PPV_ARGS(pVideoSource.GetAddressOf())));

			// Create attributes for the source reader
			ComPtr<IMFAttributes> pSourceReaderAttributes;
			ThrowIfFailed(MFCreateAttributes(pSourceReaderAttributes.GetAddressOf(), 0));

			// Create a source reader from the media source
			ThrowIfFailed(MFCreateSourceReaderFromMediaSource(pVideoSource.Get(), pSourceReaderAttributes.Get(), pSourceReader.GetAddressOf()));

			// Get the current media type of the first video stream
			ComPtr<IMFMediaType> pInputMediaType;
			ThrowIfFailed(pSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, pInputMediaType.GetAddressOf()));

			UINT32 num, den;
			ThrowIfFailed(MFGetAttributeSize(pInputMediaType.Get(), MF_MT_FRAME_SIZE, width, height));
			ThrowIfFailed(MFGetAttributeRatio(pInputMediaType.Get(), MF_MT_FRAME_RATE, &num, &den));
			*fps = static_cast<double>(num) / den;
			mf_set_default_media_type(pInputMediaType.Get(), MFVideoFormat_NV12, bitrate, *width, *height, *fps);

			ComPtr<IMFMediaType> pOutputMediaType;
			ThrowIfFailed(MFCreateMediaType(pOutputMediaType.GetAddressOf()));
			mf_set_default_media_type(pOutputMediaType.Get(), MFVideoFormat_H264, bitrate, *width, *height, *fps);

			// Create encoder
			_MFT_TYPE encoderType = mf_create_mft_video_encoder(pInputMediaType.Get(), pOutputMediaType.Get(), pEncoderTransform.GetAddressOf(), &ppEncoderActivate);
			// Create decoder
			_MFT_TYPE decoderType = mf_create_mft_video_decoder(pOutputMediaType.Get(), pInputMediaType.Get(), pDecoderTransform.GetAddressOf(), &ppDecoderActivate);

			// Apply H264 settings and update the media types
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
			// For the H264 software encoder, this message will fail initially, but for some
			// hardware encoders, it is required. So let's just ignore its HRESULT
			pEncoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL);
			// Send messages to the encoder to start streaming
			ThrowIfFailed(pEncoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
			ThrowIfFailed(pEncoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

			// Send messages to the decoder to flush data and start streaming
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
					log_info("\tStream tick.");
				}
				if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
				{
					log_info("\tEnd of stream.");
					break;
				}

				if (pVideoSample)
				{
					ThrowIfFailed(pVideoSample->SetSampleTime(llSampleTime));
					ThrowIfFailed(pVideoSample->GetSampleDuration(&llSampleDuration));

					// Encode the sample
					mf_transform_sample_to_buffer(pEncoderTransform.Get(), pVideoSample.Get(),
						[](IMFTransform* pEncoderTransform, IMFSample* pEncodedSample, void* pContext) {
#if PRINT_MBPS
							double cur_weight = 1.0 / ++_num_frames;
							DWORD bufferLength;
							ThrowIfFailed(pEncodedSample->GetTotalLength(&bufferLength));
							_avg_byte_size = bufferLength * cur_weight + _avg_byte_size * (1 - cur_weight);

							ComPtr<IMFMediaType> pInputType;
							ThrowIfFailed(pEncoderTransform->GetInputCurrentType(0, &pInputType));
							UINT32 num = 0, den = 0;
							ThrowIfFailed(MFGetAttributeRatio(pInputType.Get(), MF_MT_FRAME_RATE, &num, &den));
							double frameRate = static_cast<double>(num) / den;
							double avg_megabytes_per_second = (_avg_byte_size * frameRate) / (1024.0 * 1024.0);
							printf("\rAvg Encoding Size: %.2f MBps", avg_megabytes_per_second);
#endif
							// Decode the sample
							IMFTransform* pDecoderTransform = static_cast<IMFTransform*>(pContext);
							mf_transform_sample_to_buffer(pDecoderTransform, pEncodedSample,
								[](IMFTransform* pDecoderTransform, IMFSample* pDecodedSample, void* pContext) {
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
						}, pDecoderTransform.Get()
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
		pEncoderTransform.Reset();
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
#endif