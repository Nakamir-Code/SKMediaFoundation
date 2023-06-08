#pragma once

#include "error.h"
#include <mfapi.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <stereokit.h>

using namespace sk;
using Microsoft::WRL::ComPtr;

namespace nakamir {

	static void mf_set_default_media_type(/**[in]**/ IMFMediaType* pMediaType, const GUID& subType, UINT bitrate, UINT32 width, UINT32 height, UINT32 fps)
	{
		try
		{
			// Input media type settings
			ThrowIfFailed(pMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
			ThrowIfFailed(pMediaType->SetGUID(MF_MT_SUBTYPE, subType));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_AVG_BITRATE, bitrate));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Normal));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
			ThrowIfFailed(pMediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
			ThrowIfFailed(MFSetAttributeSize(pMediaType, MF_MT_FRAME_SIZE, width, height));
			ThrowIfFailed(MFSetAttributeRatio(pMediaType, MF_MT_FRAME_RATE, fps, 1));
			ThrowIfFailed(MFSetAttributeRatio(pMediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
		}
		catch (const std::exception& e)
		{
			log_err(e.what());
			throw e;
		}
	}
} // namespace nakamir