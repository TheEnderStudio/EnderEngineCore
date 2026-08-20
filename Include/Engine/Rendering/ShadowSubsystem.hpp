#pragma once

#include <Core/Subsystem.hpp>
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

/// @brief Cascaded shadow map subsystem wrapping Diligent's ShadowMapManager.
class EE_API ShadowSubsystem : public Subsystem {
public:
	ShadowSubsystem();
	~ShadowSubsystem() override;

	EE_NO_COPY(ShadowSubsystem)
	EE_NO_MOVE(ShadowSubsystem)

	/// @brief Attach to the main RenderSubsystem for device/context access.
	void attachToRenderer(class RenderSubsystem* renderer);

	/// @brief Configure shadows. Call before initialize().
	void setConfig(const ShadowConfig& cfg);

	/// @brief Distribute cascades each frame. Use D3D left-handed camera parameters.
	void distribute(const Vec3& lightDir, const Vec3& eye, const Vec3& center, const Vec3& up, F32 fov, F32 aspect, F32 nearP, F32 farP);

	/// @brief Get the shadow map SRV for binding in PBR shader.
	TextureSRV getSRV() const;
	TextureDSV getCascadeDSV(UInt32 cascade) const;

	/// @brief Get cascade transform (WorldToLightProjSpace matrix).
	Mat4 getCascadeTransform(UInt32 cascade) const;

	/// @brief Get pre-computed world-to-shadow-map-UV-depth matrix for a cascade.
	Mat4 getWorldToShadowMapUVDepth(UInt32 c) const;

	/// @brief Get cascade split distances in camera space (Z end for each cascade).
	Vec4 getCascadeSplitDistances() const;

	/// @brief Get shadow configuration.
	const ShadowConfig& config() const;

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
