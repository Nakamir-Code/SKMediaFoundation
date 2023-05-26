#include "mf_mp4_source_reader.h"
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

#define SAMPLE_COUNT 100
#define SOURCE_FILENAME L"Assets/demo.mp4"

using namespace nakamir;
using Microsoft::WRL::ComPtr;

/**
* Creates a new media sample and copies the first media buffer from the source to it.
* @param[in] pSrcSample: size of the media buffer to set on the create media sample.
* @param[out] pDstSample: pointer to the media sample created.
* @@Returns S_OK if successful or an error code if not.
*/
void CreateAndCopySingleBufferIMFSample(IMFSample* pSrcSample, IMFSample** pDstSample)
{
	ComPtr<IMFMediaBuffer> pDstBuffer = NULL;
	DWORD srcBufLength;

	try
	{
		// Gets total length of ALL media buffer samples. We can use here because it's only a
		// single buffer sample copy.
		ThrowIfFailed(pSrcSample->GetTotalLength(&srcBufLength));

		ThrowIfFailed(MFCreateSample(pDstSample));
		// Adds a ref count to the pDstBuffer object.
		ThrowIfFailed(MFCreateMemoryBuffer(srcBufLength, &pDstBuffer));
		// Adds another ref count to the pDstBuffer object.
		ThrowIfFailed((*pDstSample)->AddBuffer(pDstBuffer.Get()));

		// CreateSingleBufferIMFSample(srcBufLength, pDstSample);
		ThrowIfFailed(pSrcSample->CopyAllItems(*pDstSample));
		ThrowIfFailed(pSrcSample->CopyToBuffer(pDstBuffer.Get()));
	}
	catch (const std::exception& e)
	{
		log_err(e.what());
	}
}

void initialize_mf_mp4_source_reader()
{
	IMFActivate** ppActivate = NULL;

	try
	{
		ThrowIfFailed(MFStartup(MF_VERSION));

		ComPtr<IMFMediaType> pDecInputMediaType;
		ComPtr<IMFMediaType> pDecOutputMediaType;

		MF_OBJECT_TYPE ObjectType = MF_OBJECT_INVALID;
		DWORD mftStatus = 0;

		// Set up the reader for the file.
		ComPtr<IMFSourceResolver> pSourceResolver;
		ThrowIfFailed(MFCreateSourceResolver(&pSourceResolver));

		ComPtr<IUnknown> uSource;
		ThrowIfFailed(pSourceResolver->CreateObjectFromURL(
			SOURCE_FILENAME,		    // URL of the source.
			MF_RESOLUTION_MEDIASOURCE,  // Create a source object.
			NULL,                       // Optional property store.
			&ObjectType,				// Receives the created object type. 
			&uSource					// Receives a pointer to the media source.
		));

		ComPtr<IMFMediaSource> mediaFileSource;
		ThrowIfFailed(uSource->QueryInterface(IID_PPV_ARGS(&mediaFileSource)));

		ComPtr<IMFAttributes> pVideoReaderAttributes;
		ThrowIfFailed(MFCreateAttributes(&pVideoReaderAttributes, 2));

		ThrowIfFailed(pVideoReaderAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
		ThrowIfFailed(pVideoReaderAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1));

		ComPtr<IMFSourceReader> pSourceReader;
		ThrowIfFailed(MFCreateSourceReaderFromMediaSource(mediaFileSource.Get(), pVideoReaderAttributes.Get(), &pSourceReader));

		ComPtr<IMFMediaType> pFileVideoMediaType;
		ThrowIfFailed(pSourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pFileVideoMediaType));

		// get the type and subtype for the mp4 video
		GUID majorType = { 0 };
		GUID subType = { 0 };

		ThrowIfFailed(pFileVideoMediaType->GetGUID(MF_MT_MAJOR_TYPE, &majorType));
		ThrowIfFailed(pFileVideoMediaType->GetGUID(MF_MT_SUBTYPE, &subType));

		// Create H.264 decoder.
		MFT_REGISTER_TYPE_INFO inputType = { 0 };
		inputType.guidMajorType = majorType;
		inputType.guidSubtype = subType;

		UINT32 count = 0;
		ThrowIfFailed(MFTEnumEx(
			MFT_CATEGORY_VIDEO_DECODER,
			MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ALL,
			&inputType,
			NULL,
			&ppActivate,
			&count
		));

		if (count <= 0)
		{
			throw new std::exception("No hardware decoders found! :(");
		}

		std::vector<ComPtr<IMFActivate>> activateObjects(count);
		for (UINT32 i = 0; i < count; i++)
		{
			activateObjects[i].Attach(ppActivate[i]);
		}

		ComPtr<IMFTransform> pDecoderTransform; // This is H264 Decoder MFT.
		ThrowIfFailed(ppActivate[0]->ActivateObject(IID_PPV_ARGS(&pDecoderTransform)));

		MFCreateMediaType(&pDecInputMediaType);
		ThrowIfFailed(pFileVideoMediaType->CopyAllItems(pDecInputMediaType.Get()));
		ThrowIfFailed(pDecoderTransform->SetInputType(0, pDecInputMediaType.Get(), 0));

		MFCreateMediaType(&pDecOutputMediaType);
		ThrowIfFailed(pFileVideoMediaType->CopyAllItems(pDecOutputMediaType.Get()));
		ThrowIfFailed(pDecOutputMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV));

		ThrowIfFailed(pDecoderTransform->SetOutputType(0, pDecOutputMediaType.Get(), 0));

		ThrowIfFailed(pDecoderTransform->GetInputStatus(0, &mftStatus));
		if (MFT_INPUT_STATUS_ACCEPT_DATA != mftStatus)
		{
			throw new std::exception("H.264 decoder MFT is not accepting data.\n");
		}

		// log_info("H264 decoder output media type: " + GetMediaTypeDescription(pDecOutputMediaType));

		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, NULL));
		ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, NULL));

		// Start processing frames.
		IMFSample* pVideoSample = NULL, * pCopyVideoSample = NULL, * pH264DecodeOutSample = NULL;
		DWORD streamIndex, flags;
		LONGLONG llVideoTimeStamp = 0, llSampleDuration = 0;
		int sampleCount = 0;
		DWORD sampleFlags = 0;
		BOOL h264DecodeTransformFlushed = FALSE;

		while (sampleCount <= SAMPLE_COUNT)
		{
			ThrowIfFailed(pSourceReader->ReadSample(
				MF_SOURCE_READER_FIRST_VIDEO_STREAM,
				0,                              // Flags.
				&streamIndex,                   // Receives the actual stream index. 
				&flags,                         // Receives status flags.
				&llVideoTimeStamp,              // Receives the time stamp.
				&pVideoSample                    // Receives the sample or NULL.
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
				printf("Processing sample %i.\n", sampleCount);

				ThrowIfFailed(pVideoSample->SetSampleTime(llVideoTimeStamp));
				ThrowIfFailed(pVideoSample->GetSampleDuration(&llSampleDuration));
				ThrowIfFailed(pVideoSample->GetSampleFlags(&sampleFlags));

				printf("Sample count %d, Sample flags %d, sample duration %I64d, sample time %I64d\n", sampleCount, sampleFlags, llSampleDuration, llVideoTimeStamp);

				// Replicate transmitting the sample across the network and reconstructing.
				CreateAndCopySingleBufferIMFSample(pVideoSample, &pCopyVideoSample);

				// Apply the H264 decoder transform
				ThrowIfFailed(pDecoderTransform->ProcessInput(0, pCopyVideoSample, 0));

				HRESULT getOutputResult = S_OK;
				while (getOutputResult == S_OK)
				{

					// GetTransformOutput
					MFT_OUTPUT_STREAM_INFO StreamInfo = { 0 };
					MFT_OUTPUT_DATA_BUFFER outputDataBuffer = { 0 };
					DWORD processOutputStatus = 0;
					ComPtr<IMFMediaType> pChangedOutMediaType = NULL;

					h264DecodeTransformFlushed = FALSE;

					ThrowIfFailed(pDecoderTransform->GetOutputStreamInfo(0, &StreamInfo));

					outputDataBuffer.dwStreamID = 0;
					outputDataBuffer.dwStatus = 0;
					outputDataBuffer.pEvents = NULL;

					if ((StreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
					{
						ComPtr<IMFMediaBuffer> pBuffer = NULL;
						ThrowIfFailed(MFCreateSample(&pH264DecodeOutSample));
						// Adds a ref count to the pBuffer object.
						ThrowIfFailed(MFCreateMemoryBuffer(StreamInfo.cbSize, &pBuffer));
						// Adds another ref count to the pBuffer object.
						ThrowIfFailed(pH264DecodeOutSample->AddBuffer(pBuffer.Get()));
						outputDataBuffer.pSample = pH264DecodeOutSample;
					}

					auto mftProcessOutput = pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &processOutputStatus);

					//printf("Process output result %.2X, MFT status %.2X.\n", mftProcessOutput, processOutputStatus);

					if (mftProcessOutput == S_OK)
					{
						// Sample is ready and allocated on the transform output buffer.
						pH264DecodeOutSample = outputDataBuffer.pSample;
					}
					else if (mftProcessOutput == MF_E_TRANSFORM_STREAM_CHANGE)
					{
						// Format of the input stream has changed. https://docs.microsoft.com/en-us/windows/win32/medfound/handling-stream-changes
						if (outputDataBuffer.dwStatus == MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)
						{
							printf("MFT stream changed.\n");

							ThrowIfFailed(pDecoderTransform->GetOutputAvailableType(0, 0, &pChangedOutMediaType));

							// std::cout << "MFT output media type: " << GetMediaTypeDescription(pChangedOutMediaType) << std::endl << std::endl;

							ThrowIfFailed(pChangedOutMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV));
							ThrowIfFailed(pDecoderTransform->SetOutputType(0, pChangedOutMediaType.Get(), 0));
							ThrowIfFailed(pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, NULL));

							h264DecodeTransformFlushed = TRUE;
						}
						else
						{
							printf("MFT stream changed but didn't have the data format change flag set. Don't know what to do.\n");
							getOutputResult = E_NOTIMPL;
						}
					}
					else if (mftProcessOutput == MF_E_TRANSFORM_NEED_MORE_INPUT)
					{
						// More input is not an error condition but it means the allocated output sample is empty.
						getOutputResult = MF_E_TRANSFORM_NEED_MORE_INPUT;
					}
					else
					{
						printf("MFT ProcessOutput error result %.2X, MFT status %.2X.\n", mftProcessOutput, processOutputStatus);
						getOutputResult = mftProcessOutput;
					}

					if (getOutputResult != S_OK && getOutputResult != MF_E_TRANSFORM_NEED_MORE_INPUT)
					{
						throw new std::exception("Error getting H264 decoder transform output, error code %.2X.\n", getOutputResult);
					}

					if (h264DecodeTransformFlushed == TRUE)
					{
						// H264 decoder format changed. Clear the capture file and start again.
						// outputBuffer.close();
						// outputBuffer.open(CAPTURE_FILENAME, std::ios::out | std::ios::binary);
					}
					else if (pH264DecodeOutSample != NULL)
					{
						// Write decoded sample to capture file.
						// ThrowIfFailed(WriteSampleToFile(pH264DecodeOutSample, &outputBuffer));
					}
				}
				sampleCount++;
			}
		}
	}
	catch (const std::exception& e)
	{
		log_err(e.what());
	}

	if (*ppActivate)
	{
		CoTaskMemFree(ppActivate);
	}
}