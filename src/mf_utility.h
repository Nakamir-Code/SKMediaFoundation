#pragma once

#include "error.h"
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace nakamir {

	const int32_t kDefaultTargetBitrate = 5000000;

	static void mf_set_default_media_type(const ComPtr<IMFMediaType>& pMediaType, UINT32 width, UINT32 height, UINT32 fps)
	{
		try
		{
			// Input media type settings
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Normal));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_AVG_BITRATE, kDefaultTargetBitrate));
			ThrowIfFailed(MFSetAttributeSize(pMediaType.Get(), MF_MT_FRAME_SIZE, width, height));
			ThrowIfFailed(MFSetAttributeRatio(pMediaType.Get(), MF_MT_FRAME_RATE, fps, 1));
			ThrowIfFailed(MFSetAttributeRatio(pMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}
} // namespace nakamir