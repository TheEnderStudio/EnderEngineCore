#pragma once

#include <Engine/Core/Types.hpp>

EE_NAMESPACE_AUDIO_BEGIN

/// @brief Opaque handle to a decoded audio clip.
using AudioClipHandle = UInt64;

/// @brief Decoded PCM audio data.
struct AudioClip {
	Vector<F32> samples;   // interleaved PCM float samples
	UInt32 sampleRate = 0;
	UInt32 channels   = 0;
};

EE_NAMESPACE_AUDIO_END
