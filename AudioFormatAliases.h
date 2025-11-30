#pragma once

#include <spatialaudioclient.h>

// Some Windows Software Development Kit versions do not define
// SPATIAL_AUDIO_FORMAT_DOLBY_ATMOS. This alias keeps the code portable.
#ifndef SPATIAL_AUDIO_FORMAT_DOLBY_ATMOS
#define SPATIAL_AUDIO_FORMAT_DOLBY_ATMOS __uuidof(ISpatialAudioObjectRenderStream)
#endif

// Project-level alias used throughout the audio routing code.
#ifndef LGTV_SPATIAL_AUDIO_FORMAT_DOLBY_ATMOS
#define LGTV_SPATIAL_AUDIO_FORMAT_DOLBY_ATMOS SPATIAL_AUDIO_FORMAT_DOLBY_ATMOS
#endif

