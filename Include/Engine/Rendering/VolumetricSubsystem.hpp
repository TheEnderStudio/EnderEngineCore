#pragma once
/**
 * @file VolumetricSubsystem.hpp
 * @brief Single-scattering volumetric fog (height fog + shadowed sun shafts).
 */
#include <Engine/Core/Subsystem.hpp>
#include <Engine/Rendering/RenderTypes.hpp>

EE_NAMESPACE_RENDERING_BEGIN

/// @brief One frame's worth of inputs for the scattering pass.
///
/// Collected by the pipeline (which already owns the shadow cascade data and the primary
/// light) so the subsystem itself stays free of scene lookups.
struct VolumetricInput {
	TextureSRV depthSRV = nullptr;            ///< Scene depth, single sample, full resolution.
	TextureSRV shadowSRV = nullptr;           ///< Cascade shadow map (Texture2DArray).
	UInt32     depthSamples = 1;              ///< Sample count of @p depthSRV (picks the shader variant).
	/**
	 * @brief True when @p depthSRV is the framebuffer's own depth target (raster path).
	 *
	 * Reading it in a compute pass leaves it in SHADER_RESOURCE, and the next frame's scene
	 * pass then fails to bind it as the depth target ("requires texture 'Depth' to be
	 * transitioned to DEPTH_WRITE"). When set, the pass transitions it back after the
	 * dispatch. The hybrid path reads a plain resolve target, which must not be touched.
	 */
	bool   restoreDepthState = false;
	UInt32 width = 0;                     ///< Render target size (the result is full resolution).
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
	/// Local lights, as the shading paths receive them (see RenderSubsystem::getActiveLights).
	/// They are scattered without shadowing: the volumetric pass has no per-light shadow data,
	/// so a point light lights the fog around it but does not cast shafts through geometry.
	/// Tileable 3D noise for the medium's density modulation (owned by the subsystem).
	void*  noiseSRV = nullptr;
	LightConstants lights = {};
	UInt32 lightCount = 0;
	F32    timeSec = 0.0f;                ///< Drives the per-frame dither of the sampling.
};

/**
 * @brief Volumetric single scattering through an exponential height fog.
 *
 * Owns the medium's parameters and picks one of two algorithms behind an internal interface
 * (see VolumetricBackend.hpp): a screen-space view-ray march, and a froxel volume. Both
 * write `rgb = in-scattering, a = transmittance`, which the post-process chain composites as
 * `scene * transmittance + scattering` before the tone curve, so dense fog can also bloom.
 *
 * The pass runs after whatever wrote the scene depth (the raster scene, or the hybrid
 * compose) and needs only the depth, the cascade shadow map and the primary light, so it is
 * render-mode independent.
 */
class EE_API VolumetricSubsystem : public Subsystem {
public:
	VolumetricSubsystem();
	~VolumetricSubsystem() override;

	EE_NO_COPY(VolumetricSubsystem)
	EE_NO_MOVE(VolumetricSubsystem)

	/// @brief Attach to the renderer (for device/context access and the sun colour).
	void attachToRenderer(class RenderSubsystem* renderer);

	/// @brief Configure the medium, the algorithm and its quality knobs.
	void setConfig(const VolumetricConfig& cfg);
	EE_NODISCARD const VolumetricConfig& config() const;

	/// @brief Switches the algorithm; the backend is rebuilt on the next frame.
	void setMode(VolumetricConfig::Mode mode);
	EE_NODISCARD VolumetricConfig::Mode mode() const;

	/// @brief Name of the active backend, for logs and the self-check.
	EE_NODISCARD const char* backendName() const;

	/// @brief Renders the scattering/transmittance buffer for this frame.
	Result<void, CoreError> render(const VolumetricInput& input);

	/// @brief rgb = in-scattering, a = transmittance. Null when disabled or not ready.
	EE_NODISCARD void* getScatteringSRV() const;

	/// @brief Last frame's GPU time of the scattering pass, in milliseconds (0 when unknown).
	EE_NODISCARD F32 lastRenderMs() const;

	/// @brief Rolling mean of the scattering pass' GPU time, for the benchmark readout.
	EE_NODISCARD F32 averageRenderMs() const;

	/// @brief Current adaptive-quality scale (1 = the configured steps/slices, 0.5 = half).
	EE_NODISCARD F32 qualityScale() const;

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
