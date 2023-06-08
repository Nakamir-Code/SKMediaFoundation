#include "../mf_examples.h"
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

namespace nakamir {

	static IMFActivate** ppActivate = NULL;

	static std::thread sourceReaderThread;

	static ComPtr<IMFSourceReader> pSourceReader;
	static ComPtr<IMFTransform> pDecoderTransform;

	static std::atomic_bool _cancellationToken;
	static nv12_tex_t _nv12_tex;

	// PRIVATE METHODS
	static void mf_decode_source_reader_to_buffer(const ComPtr<IMFSourceReader>& pSourceReader, const ComPtr<IMFTransform>& pDecoderTransform);
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

			_VIDEO_DECODER decoderType = mf_create_mft_software_decoder(pInputMediaType.Get(), pOutputMediaType.Get(), pDecoderTransform.GetAddressOf(), &ppActivate);

			switch (decoderType)
			{
			case SOFTWARE_MFT_VIDEO_DECODER:
			{
				sourceReaderThread = std::thread(mf_decode_source_reader_to_buffer, pSourceReader, pDecoderTransform);
				break;
			}
			case D3D11_MFT_VIDEO_DECODER:
			{
				ComPtr<IMFAttributes> pAttributes;
				ThrowIfFailed(pDecoderTransform->GetAttributes(pAttributes.GetAddressOf()));
				ThrowIfFailed(pAttributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE));

				sourceReaderThread = std::thread(mf_decode_source_reader_to_buffer, pSourceReader, pDecoderTransform);
				break;
			}
			default: throw std::exception("Decoder type not found!");
			}
		}
		catch (const std::exception&)
		{
			log_err("Fatal! Failed to decode from url.");
			throw;
		}
	}

	static void mf_decode_source_reader_to_buffer(const ComPtr<IMFSourceReader>& pSourceReader, const ComPtr<IMFTransform>& pDecoderTransform)
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
				mf_decode_sample_to_buffer(pVideoSample.Get(), pDecoderTransform.Get(),
					[](byte* byteBuffer) {
						nv12_tex_set_buffer(_nv12_tex, byteBuffer);
					});
			}
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