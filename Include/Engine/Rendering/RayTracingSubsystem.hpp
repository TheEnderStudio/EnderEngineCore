#pragma once

#include <Engine/Core/Subsystem.hpp>
#include "Errors.hpp"
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

class RenderSubsystem;

/// @brief Per-frame constants for the inline ray tracing compute shader.
struct alignas(16) RayTraceConstants {
	Mat4 viewProjInv;     ///< Inverse view-projection (column-major glm::mat4).
	Vec4 lightDir;        ///< World-space light direction (toward the light, w unused).
	Vec4 cameraPos;       ///< World-space camera position.
	F32  maxRayLength = 100.0f;
	F32  ambientLight = 0.1f;
	UInt32 shadowPCF = 4; ///< PCF shadow samples per shadow ray (1..16).
	F32  lightIntensity = 1.0f; ///< Light intensity (multiplies the direct diffuse term).
	Vec4 lightColor = Vec4(1.0f, 1.0f, 1.0f, 0.0f); ///< Light color (rgb; w unused).
	Vec4 discPoints[8];   ///< 16 packed float2 disc samples (soft shadow cone, xy/zw pairs).
	Vec4 skyCorners[8];   ///< Skybox corner colors (bit0=x+, bit1=y+, bit2=z+), matches SkyboxDesc::corners.
	UInt32 skyMode = 0;   ///< 0 = corner gradient, 1 = cubemap texture.
	UInt32 _padSky[3] = { 0, 0, 0 };
	F32  aoRadius = 1.5f;      ///< AO ray length (world units).
	UInt32 aoSamples = 4;      ///< AO rays per pixel (0 disables AO).
	F32  lightSize = 0.05f;    ///< PCSS light angular size (0 = fixed-cone PCF).
	F32  reflectionBlur = 0.7f; ///< GGX reflection spread scale (0 = mirror, 1 = full roughness spread).
	UInt32 maxBounces = 1;     ///< Max reflection bounces (0 = single, 1 = two-bounce).
	F32  bounceRoughness = 0.4f; ///< Roughness threshold for the second bounce (1.0 = all surfaces).
	UInt32 reflectionSamples = 4; ///< GGX importance-sampled reflection rays per pixel (1..8).
	UInt32 frameIndex = 0;       ///< Frame counter (rotates the GGX sample pattern so the temporal denoiser averages different samples).
};

/// @brief A ray-traced scene object (mesh + material + world transform).
struct RayTracedObject {
	MeshHandle     mesh;         ///< Mesh to trace (BLAS is built from its GPU buffers).
	MaterialHandle material;     ///< Material to sample on reflection hits.
	Transform      transform;    ///< World transform (used for the TLAS instance and ObjectAttribs).
};

/**
 * @brief A ray-traced object group.
 *
 * One TLAS instance that references a single BLAS built from multiple meshes
 * (geometries), all sharing the same world transform. The order of objects
 * must match the geometry order in the BLAS; the shader selects the geometry
 * with CommittedGeometryIndex().
 */
struct RayTracedObjectGroup {
	Vector<RayTracedObject> objects; ///< Geometries (mesh + material), BLAS geometry order.
	Transform transform;             ///< Shared world transform for all geometries.
};

/**
 * @brief Ray tracing subsystem.
 *
 * Owns acceleration structures (BLAS/TLAS), bindless scene data
 * (object/material attribs + texture registry) and the inline ray tracing
 * (RayQuery, DXR 1.1) compute pipeline used to evaluate shadows and
 * reflections over the G-buffer, plus a compose pass that blends the
 * results over the scene.
 *
 * Requires a RenderSubsystem whose device reports
 * RayTracingCaps::InlineRayTracing.
 */
class EE_API RayTracingSubsystem : public Subsystem {
public:
	RayTracingSubsystem();
	~RayTracingSubsystem() override;

	EE_NO_COPY(RayTracingSubsystem)
	EE_NO_MOVE(RayTracingSubsystem)

	/// @brief Attach to the main render subsystem for device/context/mesh access.
	void attachToRenderer(RenderSubsystem* renderer);

	/// @brief Whether ray tracing is available and the subsystem is ready.
	bool isReady() const;

	/// @brief GPU time of the last trace() dispatch in milliseconds (0 if not measured yet).
	EE_NODISCARD float lastTraceMs() const;

	// -------------------------------------------------------------------
	// Acceleration structures
	// -------------------------------------------------------------------

	/// @brief Create a bottom-level acceleration structure (BLAS) from an existing mesh.
	Result<BLASHandle, RenderError> createBLAS(MeshHandle mesh);

	/// @brief Create a BLAS from multiple meshes (one geometry per mesh).
	Result<BLASHandle, RenderError> createBLAS(const Vector<MeshHandle>& meshes);

	/// @brief Create a top-level acceleration structure (TLAS).
	Result<TLASHandle, RenderError> createTLAS(UInt32 maxInstances, bool allowUpdate = true);

	/// @brief Build (first call) or update (subsequent calls) the TLAS.
	Result<void, RenderError> buildTLAS(TLASHandle tlas, const Vector<TLASInstance>& instances);

	// -------------------------------------------------------------------
	// Bindless scene
	// -------------------------------------------------------------------

	/**
	 * @brief Set the skybox used by the reflection miss shader.
	 *
	 * Must match the skybox rendered by RenderSubsystem so that reflections
	 * show the same background. Call whenever the skybox changes (and at least
	 * once before trace()).
	 * @param useCubemap true to sample the cubemap texture, false to use the
	 *                   corner gradient (trilinear interpolation).
	 * @param cubemap   Cubemap texture handle (valid only when useCubemap).
	 * @param corners   8 corner colors (bit0=x+, bit1=y+, bit2=z+), same layout
	 *                  as RenderSubsystem::SkyboxDesc::corners.
	 */
	void setSkybox(bool useCubemap, TextureHandle cubemap, const Vec4 corners[8]);

	/**
	 * @brief Rebuild bindless scene data from the given object groups.
	 *
	 * Builds/updates a BLAS per unique group mesh-set, regenerates the object
	 * and material attribute buffers and the texture registry, and rebuilds an
	 * internal TLAS (created lazily) so that trace() has a complete scene.
	 * One TLAS instance is created per group; ObjectAttribs are expanded per
	 * geometry and addressed in the shader as CustomId + CommittedGeometryIndex().
	 * @param groups Scene object groups.
	 */
	Result<void, RenderError> updateScene(const Vector<RayTracedObjectGroup>& groups);

	// -------------------------------------------------------------------
	// Ray tracing + compose
	// -------------------------------------------------------------------

	/**
	 * @brief Trace shadow + reflection rays for the current G-buffer.
	 *
	 * Reads the G-buffer (world normal + depth + color), reconstructs world
	 * space positions, casts one shadow ray toward the light and one reflection
	 * ray per pixel, and writes the result into outRT (rgba32f UAV):
	 * rgb = reflection color, a = lighting (ambient + direct). Additionally
	 * writes the RT-resolution albedo and world normal into outAlbedo/outNormal
	 * (consumed by the denoiser; they match the depth-guided texel used for
	 * lighting so the denoise features align with the color buffer).
	 * @param c             Per-frame constants (view-proj inverse, light, camera).
	 * @param gBufferNormal SRV of the G-buffer world normal (ITextureView* as void*).
	 * @param gBufferDepth  SRV of the G-buffer depth (ITextureView* as void*).
	 * @param gBufferColor  SRV of the G-buffer albedo (ITextureView* as void*).
	 * @param outRT         UAV of the ray traced output texture (ITextureView* as void*).
	 * @param outAlbedo     UAV of the RT-res albedo (ITextureView* as void*).
	 * @param outNormal     UAV of the RT-res world normal (ITextureView* as void*).
	 * @param width,height  G-buffer dimensions.
	 */
	Result<void, RenderError> trace(const RayTraceConstants& c,
		void* gBufferNormal, void* gBufferDepth, void* gBufferColor,
		void* outRT, void* outAlbedo, void* outNormal,
		UInt32 width, UInt32 height);

	/**
	 * @brief Denoise the ray traced output.
	 *
	 * When an Open Image Denoise GPU device with D3D12 external-memory import is
	 * available, the frame is denoised entirely on the GPU (zero-copy shared
	 * buffers, no CPU participation). Otherwise a temporal + spatial (SVGF-lite)
	 * compute filter is used. compose() should then read getDenoisedSRV().
	 * @param rtSRV           SRV of the raw ray traced output (from trace()).
	 * @param gBufferAlbedoSRV SRV of the single-sample G-buffer albedo (OIDN input).
	 * @param gBufferNormalSRV SRV of the G-buffer world normal.
	 * @param gBufferDepthSRV  SRV of the G-buffer depth (temporal path only).
	 * @param viewProjInv     Inverse view-projection of the current frame.
	 * @param viewProj        View-projection of the current frame (temporal path only).
	 * @param width,height    RT output dimensions (dispatch size).
	 */
	Result<void, RenderError> denoise(void* rtSRV, void* gBufferAlbedoSRV, void* gBufferNormalSRV, void* gBufferDepthSRV,
		const Mat4& viewProjInv, const Mat4& viewProj, UInt32 width, UInt32 height);

	/// @brief Whether the Open Image Denoise GPU path is active (all-GPU denoising).
	EE_NODISCARD bool oidnActive() const;

	/// @brief SRV of the latest denoised frame (nullptr until denoise() runs).
	void* getDenoisedSRV() const;

	/// @brief Set the temporal history weight (0 = no history, 1 = full history).
	void setDenoiseStrength(F32 historyWeight);

	/// @brief Choose the OIDN pipeline when it is available: async (1-frame
	///        latency, denoise overlaps with rendering) or sync (0 latency,
	///        denoise serialized with rendering). Ignored when OIDN is off.
	void setOIDNAsync(bool enable);

	/**
	 * @brief Compose G-buffer + ray traced results over the scene.
	 *
	 * Draws a fullscreen triangle that blends the shaded albedo with the
	 * reflection (Schlick Fresnel) into outputRTV. Background pixels (depth
	 * == 1) are emitted with alpha 0 so the previously rendered skybox is
	 * preserved (the compose PSO uses alpha blending).
	 * @param gBufferColor  SRV of the G-buffer albedo.
	 * @param gBufferNormal SRV of the G-buffer world normal.
	 * @param gBufferDepth  SRV of the G-buffer depth.
	 * @param gBufferEmissive SRV of the G-buffer emissive (added untinted after
	 *                        the Fresnel blend).
	 * @param rtTex         SRV of the ray traced output (from trace()).
	 * @param outputRTV     RTV to compose into (e.g. the HDR target).
	 * @param depthDSV      Optional DSV to keep bound after composing (pass the
	 *                      scene/G-buffer depth so subsequent 2D/UI passes that
	 *                      expect a D32 depth stay consistent); nullptr unbinds it.
	 * @param drawMode      Debug render mode: 0=shaded, 1=G-buffer color,
	 *                      2=G-buffer normal, 3=diffuse lighting, 4=reflections, 5=Fresnel.
	 * @param viewProjInv   Inverse view-projection (column-major).
	 * @param cameraPos     World-space camera position.
	 * @param lightColor    Light color (applied to the diffuse term).
	 * @param width,height  Target dimensions.
	 */
	Result<void, RenderError> compose(void* gBufferColor, void* gBufferNormal, void* gBufferDepth,
		void* gBufferEmissive, void* rtTex, void* outputRTV, void* depthDSV, UInt32 drawMode,
		const Mat4& viewProjInv, const Vec3& cameraPos, const Vec3& lightColor,
		UInt32 width, UInt32 height);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	bool tryInitOIDN(UInt32 width, UInt32 height);      ///< Lazy-init the OIDN GPU path (once).
	void shutdownOIDN();                                 ///< Release all OIDN/D3D12 shared resources.
	void oidnDenoise(void* rtSRV, void* albedoSRV, void* normalSRV, UInt32 width, UInt32 height);
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
