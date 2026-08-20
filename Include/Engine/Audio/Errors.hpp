#pragma once

#include <Core/Types.hpp>

EE_NAMESPACE_AUDIO_BEGIN

/// @brief Error codes for audio subsystem operations.
enum class AudioError { None, InitFailed, DecodeFailed, PlayFailed };

/// @brief Convert an AudioError to a human-readable string.
inline const char* ToString(AudioError e) {
	switch (e) {
	case AudioError::None: return "None";
	case AudioError::InitFailed: return "InitFailed";
	case AudioError::DecodeFailed: return "DecodeFailed";
	case AudioError::PlayFailed: return "PlayFailed";
	}
	return "Unknown";
}

EE_NAMESPACE_AUDIO_END
