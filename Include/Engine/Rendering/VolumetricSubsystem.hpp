#pragma once
/**
 * @file VolumetricSubsystem.hpp
 * @brief Single-scattering volumetric fog (height fog + shadowed sun shafts).
 */
#include <Core/Subsystem.hpp>
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

/// @brief One frame's worth of inputs for the scattering pass.
///
/// Collected by the pipeline (which already owns the shadow cascade data and the
/// primary light) so the subsystem itself stays free of scene lookups.
struct VolumetricInput {
	void*  depthSRV = nullptr;            ///< Scene depth, single sample, full resolution.
	void*  shadowSRV = nullptr;           ///< Cascade shadow map (Texture2DArray).
	UInt32 depthSamples = 1;              ///< Sample count of @p depthSRV (picks the shader variant).
	/**
	 * @brief True when @p depthSRV is the framebuffer's own depth target (raster path).
	 *
	 * Reading it in a compute pass leaves it in SHADER_RESOURCE, and the next frame's scene
	 * pass then fails to bind it as the depth target ("requires texture 'Depth' to be
	 * transitioned to DEPTH_WRITE"). When set, the pass transitions it back after the
	 * dispatch. The hybrid path reads a plain resolve target, which must not be touched.
	 */
	bool restoreDepthState = false;
	UInt32 width = 0;                     ///< Render target size (the march runs at this size).
	UInt32 height = 0;
	Mat4   viewProj = Mat4(1.0f);         ///< Camera view-projection (for the cascade selection).
	Mat4   viewProjInv = Mat4(1.0f);      ///< Inverse, for the world-space reconstruction.
	Vec3   cameraPos = Vec3(0.0f);
	Vec3   sunDir = Vec3(0.0f, -1.0f, 0.0f); ///< Direction the light travels (same convention as shadows).
	Vec4   sunColor = Vec4(1.0f);         ///< rgb = colour, w = intensity.
	Mat4   shadowUVDepth[4] = {};         ///< World -> shadow map UV/depth, per cascade.
	Vec4   cascadeSplits = Vec4(0.0f);    ///< Camera-space Z end of each cascade.
	Vec4   skyCorners[8] = {};            ///< Sky irradiance for the ambient scattering term.
	Vec4   ambient = Vec4(0.0f);          ///< rgb = ambient colour, a = strength.
	F32    timeSec = 0.0f;                ///< Drives the march's per-frame dither.
};

/**
 * @brief Volumetric single scattering through an exponential height fog.
 *
 * One compute pass marches each pixel's view ray from the camera to the scene depth (or to
 * the configured maximum distance for the sky) and accumulates in-scattered light, with the
 * sun's visibility taken from the cascaded shadow map - that shadowing is what produces the
 * shafts. The result is written as rgb = in-scattering and a = transmittance, which the
 * post-process chain composites as `scene * transmittance + scattering` before bloom, so
 * dense fog can also bloom.
 *
 * V1 of the volumetric plan: no temporal accumulation and no local lights yet (see the
 * plan's V2/V3), so the march runs at full resolution and the knobs are read every frame.
 */
class EE_API VolumetricSubsystem : public Subsystem {
public:
	VolumetricSubsystem();
	~VolumetricSubsystem() override;

	EE_NO_COPY(VolumetricSubsystem)
	EE_NO_MOVE(VolumetricSubsystem)

	/// @brief Attach to the renderer (for device/context access and the sun colour).
	void attachToRenderer(class RenderSubsystem* renderer);

	/// @brief Configure the medium and the march quality.
	void setConfig(const VolumetricConfig& cfg);
	EE_NODISCARD const VolumetricConfig& config() const;

	/// @brief Renders the scattering/transmittance buffer for this frame.
	Result<void, CoreError> render(const VolumetricInput& input);

	/// @brief rgb = in-scattering, a = transmittance. Null when disabled or not ready.
	EE_NODISCARD void* getScatteringSRV() const;

	/// @brief Last frame's GPU time of the march, in milliseconds (0 when unavailable).
	EE_NODISCARD F32 lastRenderMs() const;

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
