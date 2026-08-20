#pragma once

#include <Core/Subsystem.hpp>
#include "Errors.hpp"
#include "AudioTypes.hpp"

EE_NAMESPACE_AUDIO_BEGIN

/// @brief Audio subsystem providing decoding, 3D spatial playback, and SteamAudio scene integration.
class EE_API AudioSubsystem : public Subsystem {
public:
	/// @brief Construct the audio subsystem.
	AudioSubsystem();
	/// @brief Destroy the audio subsystem.
	~AudioSubsystem() override;

	EE_NO_COPY(AudioSubsystem)
	EE_NO_MOVE(AudioSubsystem)

	/// @brief Decode an audio file from disk into PCM samples.
	Result<AudioClip, AudioError> decode(const String& path);

	/// @brief Decode from a resource archive.
	Result<AudioClip, AudioError> decode(const ResPath& path);

	/// @brief Play a decoded audio file at a 3D position.
	void playFile(const String& path, const Vec3& pos, F32 volume = 1.0f);

	/// @brief Play a sound from a resource archive.
	void playFile(const ResPath& path, const Vec3& pos, F32 volume = 1.0f);

	/// @brief Set the listener position and orientation for 3D audio.
	void setListener(const Vec3& pos, const Vec3& fwd, const Vec3& up);

	/// @brief Bind a PhysX PxScene* for occlusion tracing via SteamAudio IPLScene callbacks.
	void bindScene(void* pxScene);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;
	void onUpdate(F64) override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_AUDIO_END
