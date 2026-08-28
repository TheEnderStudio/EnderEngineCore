#pragma once

#include <Engine/Core/Subsystem.hpp>
#include "Errors.hpp"
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

class RenderSubsystem;

/// @brief Per-frame constants for the inline ray tracing compute shader.
struct alignas(16) RayTraceConstants {
	Mat4 viewProjInv;     ///< Inverse view-projection (column-major glm::mat4).
	Vec4 lightDir;        ///< World-space light direction (xyz, w unused).
	Vec4 cameraPos;       ///< World-space camera position.
	F32  maxRayLength = 100.0f;
	F32  ambientLight = 0.1f;
	F32  _pad0 = 0.0f;
	F32  _pad1 = 0.0f;
};

/// @brief A ray-traced scene object (mesh + material + world transform).
struct RayTracedObject {
	MeshHandle     mesh;         ///< Mesh to trace (BLAS is built from its GPU buffers).
	MaterialHandle material;     ///< Material to sample on reflection hits.
	Transform      transform;    ///< World transform (used for the TLAS instance and ObjectAttribs).
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

	// -------------------------------------------------------------------
	// Acceleration structures
	// -------------------------------------------------------------------

	/// @brief Create a bottom-level acceleration structure (BLAS) from an existing mesh.
	Result<BLASHandle, RenderError> createBLAS(MeshHandle mesh);

	/// @brief Create a top-level acceleration structure (TLAS).
	Result<TLASHandle, RenderError> createTLAS(UInt32 maxInstances, bool allowUpdate = true);

	/// @brief Build (first call) or update (subsequent calls) the TLAS.
	Result<void, RenderError> buildTLAS(TLASHandle tlas, const Vector<TLASInstance>& instances);

	// -------------------------------------------------------------------
	// Bindless scene
	// -------------------------------------------------------------------

	/**
	 * @brief Rebuild bindless scene data from the given render list.
	 *
	 * Builds/updates a BLAS per unique mesh, regenerates the object and
	 * material attribute buffers and the texture registry, and rebuilds an
	 * internal TLAS (created lazily) so that trace() has a complete scene.
	 * @param objects Scene objects (mesh + material + transform).
	 */
	Result<void, RenderError> updateScene(const Vector<RayTracedObject>& objects);

	// -------------------------------------------------------------------
	// Ray tracing + compose
	// -------------------------------------------------------------------

	/**
	 * @brief Trace shadow + reflection rays for the current G-buffer.
	 *
	 * Reads the G-buffer (world normal + depth), reconstructs world space
	 * positions, casts one shadow ray toward the light and one reflection
	 * ray per pixel, and writes the result into outRT (rgba16f UAV):
	 * rgb = reflection color, a = lighting (max(ambient, NdotL)).
	 * @param c             Per-frame constants (view-proj inverse, light, camera).
	 * @param gBufferNormal SRV of the G-buffer world normal (ITextureView* as void*).
	 * @param gBufferDepth  SRV of the G-buffer depth (ITextureView* as void*).
	 * @param outRT         UAV of the ray traced output texture (ITextureView* as void*).
	 * @param width,height  G-buffer dimensions.
	 */
	Result<void, RenderError> trace(const RayTraceConstants& c,
		void* gBufferNormal, void* gBufferDepth, void* outRT,
		UInt32 width, UInt32 height);

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
	 * @param rtTex         SRV of the ray traced output (from trace()).
	 * @param outputRTV     RTV to compose into (e.g. the HDR target).
	 * @param depthDSV      Optional DSV to keep bound after composing (pass the
	 *                      scene/G-buffer depth so subsequent 2D/UI passes that
	 *                      expect a D32 depth stay consistent); nullptr unbinds it.
	 * @param viewProjInv   Inverse view-projection (column-major).
	 * @param cameraPos     World-space camera position.
	 * @param width,height  Target dimensions.
	 */
	Result<void, RenderError> compose(void* gBufferColor, void* gBufferNormal, void* gBufferDepth,
		void* rtTex, void* outputRTV, void* depthDSV,
		const Mat4& viewProjInv, const Vec3& cameraPos,
		UInt32 width, UInt32 height);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
