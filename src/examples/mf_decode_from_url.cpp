#include "../mf_examples.h"
#include "../mf_decoder.h"
#include "../mf_encoder.h"
#include "../error.h"
#include <wrl/client.h>
#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <atomic>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace nakamir {

	IMFActivate** ppActivate = NULL;

	std::thread sourceReaderThread;

	ComPtr<IMFSourceReader> pSourceReader;
	ComPtr<IMFTransform> pDecoderTransform; // This is the H264 Decoder MFT

	std::atomic_bool _cancellationToken;
	nv12_tex_t _nv12_tex;

	// PRIVATE METHODS
	static void decode_source_reader_cpu(IMFSourceReader* pSourceReader, IMFTransform* pDecoderTransform);
	static void decode_source_reader_gpu(IMFSourceReader* pSourceReader);
	static void reset_decoder();
	static void mf_shutdown_thread();

	void mf_decode_from_url(const wchar_t* filename, nv12_tex_t nv12_tex, std::function<void()>* shutdown_thread)
	{
		*shutdown_thread = mf_shutdown_thread;
		_cancellationToken = false;
		_nv12_tex = nv12_tex;
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
			ThrowIfFailed(uSource->QueryInterface(IID_PPV_ARGS(&mediaFileSource)));

			// Create attributes for the source reader
			ComPtr<IMFAttributes> pVideoReaderAttributes;
			ThrowIfFailed(MFCreateAttributes(&pVideoReaderAttributes, 0));

			// Create a source reader from the media source
			ThrowIfFailed(MFCreateSourceReaderFromMediaSource(mediaFileSource.Get(), pVideoReaderAttributes.Get(), &pSourceReader));

			// Get the current media type of the first video stream
			ComPtr<IMFMediaType> pFileVideoMediaType;
			ThrowIfFailed(pSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pFileVideoMediaType));

			_VIDEO_DECODER decoderType = mf_create_mft_software_decoder(pFileVideoMediaType.Get(), &pDecoderTransform, &ppActivate);
			mf_print_stream_info(pDecoderTransform.Get());

			switch (decoderType)
			{
			case SOFTWARE_MFT_VIDEO_DECODER:
				sourceReaderThread = std::thread(decode_source_reader_cpu, pSourceReader.Get(), pDecoderTransform.Get());
				break;
			case D3D_VIDEO_DECODER:
				sourceReaderThread = std::thread(decode_source_reader_cpu, pSourceReader.Get(), pDecoderTransform.Get());
				break;
			default: throw std::exception("Decoder type not found!");
			}
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}

	static void decode_source_reader_cpu(IMFSourceReader* pSourceReader, IMFTransform* pDecoderTransform)
	{
		// Send messages to decoder to flush data and start streaming
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

		// Start processing frames
		LONGLONG llSampleTime = 0, llSampleDuration = 0;
		DWORD sampleFlags = 0;

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
				&pVideoSample                   // Receives the sample or NULL.
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
				mf_decode_sample_cpu(pVideoSample.Get(), pDecoderTransform, [](byte* byteBuffer) {
					nv12_tex_set_buffer(_nv12_tex, byteBuffer);
					});
			}
		}

		reset_decoder();
	}

	static void decode_source_reader_gpu(IMFSourceReader* pSourceReader)
	{
		// Start processing frames
		LONGLONG llVideoTimeStamp = 0, llSampleDuration = 0;
		DWORD sampleFlags = 0;

		while (!_cancellationToken)
		{
			ComPtr<IMFSample> pVideoSample;
			DWORD streamIndex, flags;
			ThrowIfFailed(pSourceReader->ReadSample(
				MF_SOURCE_READER_FIRST_VIDEO_STREAM,
				0,                              // Flags.
				&streamIndex,                   // Receives the actual stream index. 
				&flags,                         // Receives status flags.
				&llVideoTimeStamp,              // Receives the time stamp.
				&pVideoSample                   // Receives the sample or NULL.
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
				//mf_decode_sample_gpu(pVideoSample.Get(), pDecoderTransform.Get());
			}
		}

		reset_decoder();
	}

	static void reset_decoder()
	{
		pSourceReader.Reset();
		pDecoderTransform.Reset();

		if (ppActivate && *ppActivate)
		{
			CoTaskMemFree(ppActivate);
		}
	}

	static void mf_shutdown_thread()
	{
		_cancellationToken = true;
		sourceReaderThread.join();
	}
} // namespace nakamir