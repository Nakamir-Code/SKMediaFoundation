#include "mf_decode_to_buffer.h"
#include "error.h"
#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <vector>

EXTERN_GUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, 0xc60ac5fe, 0x252a, 0x478f, 0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3);
EXTERN_GUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID, 0x8ac3587a, 0x4ae7, 0x42d8, 0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f);

#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace nakamir {
	void mf_mp4_source_reader(const wchar_t* filename, nv12_tex_t nv12_tex)
	{
		IMFActivate** ppActivate = NULL;

		try
		{
			// Start up the Media Foundation platform
			ThrowIfFailed(MFStartup(MF_VERSION));

			// Set up the reader for the file
			ComPtr<IMFSourceResolver> pSourceResolver;
			ThrowIfFailed(MFCreateSourceResolver(&pSourceResolver));

			// Create a media source object from a URL
			MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;
			ComPtr<IUnknown> uSource;
			ThrowIfFailed(pSourceResolver->CreateObjectFromURL(
				filename,					// URL of the source.
				MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ,  // Create a source object.
				NULL,                       // Optional property store.
				&ObjectType,				// Receives the created object type. 
				&uSource					// Receives a pointer to the media source.
			));

			// Get a pointer to the media source
			ComPtr<IMFMediaSource> mediaFileSource;
			ThrowIfFailed(uSource->QueryInterface(IID_PPV_ARGS(&mediaFileSource)));

			// Create attributes for the source reader
			ComPtr<IMFAttributes> pVideoReaderAttributes;
			ThrowIfFailed(MFCreateAttributes(&pVideoReaderAttributes, 2));

			// Set attributes for the source reader
			ThrowIfFailed(pVideoReaderAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
			ThrowIfFailed(pVideoReaderAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1));

			// Create a source reader from the media source
			ComPtr<IMFSourceReader> pSourceReader;
			ThrowIfFailed(MFCreateSourceReaderFromMediaSource(mediaFileSource.Get(), pVideoReaderAttributes.Get(), &pSourceReader));

			// Get the current media type of the first video stream
			ComPtr<IMFMediaType> pFileVideoMediaType;
			ThrowIfFailed(pSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pFileVideoMediaType));

			// Get the major and subtype of the mp4 video
			GUID majorType = { 0 };
			GUID subType = { 0 };
			ThrowIfFailed(pFileVideoMediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType));
			ThrowIfFailed(pFileVideoMediaType->GetGUID(MF_MT_SUBTYPE, &subType));

			// Create H.264 decoder.
			MFT_REGISTER_TYPE_INFO inputType = { 0 };
			inputType.guidMajorType = majorType;
			inputType.guidSubtype = subType;

			// Enumerate decoders that match the input type
			UINT32 count = 0;
			ThrowIfFailed(MFTEnumEx(
				MFT_CATEGORY_VIDEO_DECODER,
				MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ALL,
				&inputType,
				NULL,
				&ppActivate,
				&count
			));

			// Check if any decoders were found
			if (count <= 0)
			{
				throw new std::exception("No decoders found! :(");
			}

			std::vector<ComPtr<IMFActivate>> activateObjects(count);
			for (UINT32 i = 0; i < count; i++)
			{
				activateObjects[i].Attach(ppActivate[i]);

				LPWSTR pszName = nullptr;
				UINT32 pszLength;
				if (FAILED(ppActivate[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &pszName, &pszLength)))
				{
					continue;
				}

				OutputDebugStringW(pszName);
				CoTaskMemFree(pszName);
			}

			// Activate first decoder object and get a pointer to it
			ComPtr<IMFTransform> pDecoderTransform; // This is the H264 Decoder MFT
			ThrowIfFailed(ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pDecoderTransform)));

			// Create input media type for decoder and copy all items from file video media type
			ComPtr<IMFMediaType> pInputMediaType;
			MFCreateMediaType(&pInputMediaType);
			ThrowIfFailed(pFileVideoMediaType->CopyAllItems(pInputMediaType.Get()));
			ThrowIfFailed(pDecoderTransform->SetInputType(0, pInputMediaType.Get(), 0));

			// Create output media type for decoder and copy all items from file video media type
			ComPtr<IMFMediaType> pOutputMediaType;
			MFCreateMediaType(&pOutputMediaType);
			ThrowIfFailed(pFileVideoMediaType->CopyAllItems(pOutputMediaType.Get()));
			ThrowIfFailed(pOutputMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));

			// Set output type for decoder
			ThrowIfFailed(pDecoderTransform->SetOutputType(0, pOutputMediaType.Get(), 0));

			DWORD mftStatus = 0;
			ThrowIfFailed(pDecoderTransform->GetInputStatus(0, &mftStatus));
			if (MFT_INPUT_STATUS_ACCEPT_DATA != mftStatus)
			{
				throw new std::exception("H.264 decoder MFT is not accepting data.\n");
			}

			// log_info("H264 decoder output media type: " + GetMediaTypeDescription(pDecOutputMediaType));

			// Send messages to decoder to flush data and start streaming
			ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
			ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
			ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

			// Start processing frames.
			LONGLONG llVideoTimeStamp = 0, llSampleDuration = 0;
			int sampleCount = 0;
			DWORD sampleFlags = 0;

			while (true)
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
				if (flags & MF_SOURCE_READERF_NEWSTREAM)
				{
					printf("\tNew stream.\n");
					break;
				}
				if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)
				{
					printf("\tNative type changed.\n");
					break;
				}
				if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
				{
					printf("\tCurrent type changed.\n");
					break;
				}

				if (pVideoSample)
				{
					//printf("Processing sample %i.\n", sampleCount);

					ThrowIfFailed(pVideoSample->SetSampleTime(llVideoTimeStamp));
					ThrowIfFailed(pVideoSample->GetSampleDuration(&llSampleDuration));
					ThrowIfFailed(pVideoSample->GetSampleFlags(&sampleFlags));

					//printf("Sample count %d, Sample flags %d, sample duration %I64d, sample time %I64d\n", sampleCount, sampleFlags, llSampleDuration, llVideoTimeStamp);

					// Gets total length of ALL media buffer samples. We can use here because it's only a
					// single buffer sample copy.
					DWORD srcBufLength;
					ThrowIfFailed(pVideoSample->GetTotalLength(&srcBufLength));

					ComPtr<IMFSample> pCopyVideoSample;
					ThrowIfFailed(MFCreateSample(&pCopyVideoSample));

					// Adds a ref count to the pDstBuffer object.
					ComPtr<IMFMediaBuffer> pDstBuffer;
					ThrowIfFailed(MFCreateMemoryBuffer(srcBufLength, &pDstBuffer));
					// Adds another ref count to the pDstBuffer object.
					ThrowIfFailed(pCopyVideoSample->AddBuffer(pDstBuffer.Get()));

					ThrowIfFailed(pVideoSample->CopyAllItems(pCopyVideoSample.Get()));
					ThrowIfFailed(pVideoSample->CopyToBuffer(pDstBuffer.Get()));

					// Apply the H264 decoder transform
					ThrowIfFailed(pDecoderTransform->ProcessInput(0, pCopyVideoSample.Get(), 0));

					MFT_OUTPUT_STREAM_INFO StreamInfo = { 0 };
					ThrowIfFailed(pDecoderTransform->GetOutputStreamInfo(0, &StreamInfo));

					MFT_OUTPUT_DATA_BUFFER outputDataBuffer = { 0 };
					outputDataBuffer.dwStreamID = 0;
					outputDataBuffer.dwStatus = 0;
					outputDataBuffer.pEvents = NULL;
					outputDataBuffer.pSample = NULL;

					HRESULT mftProcessOutput = S_OK;
					while (mftProcessOutput == S_OK)
					{
						ComPtr<IMFSample> pH264DecodeOutSample;

						if ((StreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
						{
							ThrowIfFailed(MFCreateSample(&pH264DecodeOutSample));

							ComPtr<IMFMediaBuffer> pBuffer;
							ThrowIfFailed(MFCreateMemoryBuffer(StreamInfo.cbSize, &pBuffer));

							ThrowIfFailed(pH264DecodeOutSample->AddBuffer(pBuffer.Get()));

							outputDataBuffer.pSample = pH264DecodeOutSample.Get();
						}

						DWORD processOutputStatus = 0;
						mftProcessOutput = pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &processOutputStatus);

						//printf("Process output result %.2X, MFT status %.2X.\n", mftProcessOutput, processOutputStatus);

						if (mftProcessOutput == S_OK && pH264DecodeOutSample != NULL)
						{
							// Write the decoded sample to the nv12 texture.
							ComPtr<IMFMediaBuffer> buffer;
							ThrowIfFailed(pH264DecodeOutSample->ConvertToContiguousBuffer(&buffer));

							DWORD bufferLength;
							ThrowIfFailed(buffer->GetCurrentLength(&bufferLength));

							//printf("Sample size %i.\n", bufferLength);

							byte* byteBuffer = NULL;
							DWORD maxLength = 0, currentLength = 0;
							ThrowIfFailed(buffer->Lock(&byteBuffer, &maxLength, &currentLength));

							nv12_tex_set_buffer(nv12_tex, byteBuffer);

							ThrowIfFailed(buffer->Unlock());
						}

						// More input is not an error condition but it means the allocated output sample is empty.
						if (mftProcessOutput != S_OK && mftProcessOutput != MF_E_TRANSFORM_NEED_MORE_INPUT)
						{
							printf("MFT ProcessOutput error result %.2X, MFT status %.2X.\n", mftProcessOutput, processOutputStatus);
							throw new std::exception("Error getting H264 decoder transform output, error code %.2X.\n", mftProcessOutput);
						}
					}
					sampleCount++;
				}
			}

			MFShutdown();
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
		}

		if (ppActivate && *ppActivate)
		{
			CoTaskMemFree(ppActivate);
		}
	}
} // namespace nakamir