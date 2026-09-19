#pragma once

#include <Engine/Core/Subsystem.hpp>
#include <Engine/Core/Types.hpp>
#include "Errors.hpp"
#include "RenderTypes.hpp"

EE_NAMESPACE_RENDERING_BEGIN

class RenderSubsystem;

/**
 * @brief Amplification + Mesh shader rendering subsystem.
 *
 * Implements a GPU-driven mesh rendering path: an amplification shader culls
 * instances (frustum) and selects a LOD level per mesh, a mesh shader emits the
 * visible geometry, all without CPU readback / indirect-arg rewriting.
 *
 * Development stages:
 *   M0 - device capability probing (subsystem reports device support).
 *   M1 - minimal AS/MS pipeline: a Tutorial20-style grid of animated cubes
 *        (whole-cube mesh tasks, frustum culling, screen-space LOD coloring,
 *        GPU visible-count statistics) drawn with DrawMesh into the current
 *        render target (M1 validates the pipeline end-to-end).
 *   M2 - meshletization of real engine meshes + per-instance culling.
 *   M3 - real LOD chains (per-mesh simplified geometry) selected in the AS.
 */
class EE_API MeshShaderSubsystem final : public Subsystem {
public:
	MeshShaderSubsystem();
	~MeshShaderSubsystem() override;

	void attachToRenderer(RenderSubsystem* r);

	/// @brief Whether the subsystem is usable (renderer attached + device supports mesh shaders).
	EE_NODISCARD bool isReady() const;

	/// @brief Enable/disable frustum culling in the amplification shader (M1 test grid).
	void setFrustumCulling(bool enable);

	/// @brief Set the LOD selection scale for the scene path (multiplies the projected size; lower = more LOD detail).
	void setLodScale(F32 scale);

	/// @brief Hard cap on the mesh groups a single amplification group may dispatch (GPU overload protection).
	void setMeshletCap(UInt32 cap);

	/// @brief Diagnostic: when enabled the mesh shader emits a fixed triangle without reading geometry buffers.
	void setDebugMode(UInt32 mode);

	/// @brief Diagnostic: draw only the given mesh id (0xFFFFFFFF = all meshes).
	void setMeshFilter(UInt32 meshId);

	/// @brief Number of visible instances from the last test-grid frame (fence readback, one frame delayed).
	EE_NODISCARD UInt32 lastVisibleCount() const;

	// ------------------------------------------------------------------
	// M2: GPU-driven real meshes (meshlet pipeline)
	// ------------------------------------------------------------------

	/**
	 * @brief Register the meshes rendered by the mesh shader path.
	 *
	 * CPU-builds meshlets from the meshes' CPU vertex/index data (kept by the
	 * renderer), concatenates the geometry into GPU pools and prepares the
	 * static mesh-instance task list. Call once after the meshes are loaded.
	 */
	Result<void, RenderError> setMeshes(const Vector<MeshHandle>& meshes);

	/**
	 * @brief Draw every registered mesh once per instance matrix, GPU-driven.
	 *
	 * The amplification shader frustum-culls each (mesh, instance) task, the
	 * mesh shader emits the visible meshlets. One DrawMesh for all instances.
	 * @param instanceMatrices Per-instance world matrices (size N).
	 * @param view      World->view matrix.
	 * @param proj      View->clip matrix.
	 * @param timeSec   Time in seconds.
	 */
	Result<void, RenderError> drawScene(const Vector<Mat4>& instanceMatrices, const Mat4& view, const Mat4& proj, F32 timeSec);

	/**
	 * @brief Same draw, but into the hybrid ray tracing G-buffer.
	 *
	 * Uses an identical amplification/mesh shader pair (so culling and cluster
	 * LOD behave exactly as in drawScene) with a pixel shader that writes the
	 * three G-buffer targets instead of shaded colour: albedo (RGBA8_SRGB),
	 * world normal + roughness (RGBA16F) and emissive (RGBA16F). The caller must
	 * have bound those targets first (see RenderSubsystem::beginGBuffer); this
	 * call must be made between beginGBuffer() and endGBuffer().
	 */
	Result<void, RenderError> drawSceneGBuffer(const Vector<Mat4>& instanceMatrices, const Mat4& view, const Mat4& proj, F32 timeSec);

	/**
	 * @brief One instanced draw group: registered meshes drawn with one instance list.
	 *
	 * Every mesh in a group must share the same local space (they are transformed
	 * by the same instance matrices), which is what lets the group get a single
	 * bounding sphere for culling. Typically one group per scene object type:
	 * terrain, walls, one group per animated model, and so on.
	 */
	struct MeshDrawGroup {
		Vector<UInt32> meshIds;          ///< Indices into the list passed to setMeshes().
		Vector<Mat4>   instanceMatrices; ///< World matrix per instance (max 4096 total).
	};

	/**
	 * @brief Draw several instanced groups in one GPU-driven pass.
	 *
	 * All groups are culled by one amplification dispatch (one DrawMesh), each
	 * task carrying its own group so culling and the too-small drop test use that
	 * object's bounding sphere rather than a single scene-wide one. Cluster LOD
	 * applies to every group.
	 */
	Result<void, RenderError> drawScene(const Vector<MeshDrawGroup>& groups, const Mat4& view, const Mat4& proj, F32 timeSec);

	/// @brief Grouped variant of drawSceneGBuffer (hybrid ray tracing G-buffer).
	Result<void, RenderError> drawSceneGBuffer(const Vector<MeshDrawGroup>& groups, const Mat4& view, const Mat4& proj, F32 timeSec);

	/// @brief Visible (mesh, instance) tasks from the last drawScene frame (fence readback).
	EE_NODISCARD UInt32 lastSceneVisibleTasks() const;

	/// @brief Visible clusters actually dispatched last frame (fence readback).
	EE_NODISCARD UInt32 lastSceneVisibleClusters() const;

	/// @brief Clusters wanted last frame before the per-frame budget clamp was applied.
	EE_NODISCARD UInt32 lastSceneClusterDemand() const;

	/**
	 * @brief Hard per-frame cap on the number of cluster mesh groups dispatched.
	 *
	 * The amplification shader stops appending to the visible-cluster list once
	 * the budget is exhausted, so a single DrawMesh can never ask the driver for
	 * an unbounded number of derived mesh groups (which hangs/TDRs the device).
	 * @param budget Maximum visible clusters per frame (clamped to [256, 1<<18]).
	 */
	void setClusterBudget(UInt32 budget);

	/**
	 * @brief Shadow map + cascades used by the raster pixel shader.
	 *
	 * The mesh shader path shades with the engine's forward model, which includes
	 * the cascaded shadow term, so it needs the same data the forward renderer
	 * binds per draw. Call once per frame after the shadow pass and before
	 * drawScene(); without it every object is lit as if nothing occluded it.
	 * @param shadowMap       Shadow map SRV (a Texture2DArray), or nullptr to disable.
	 * @param worldToShadowUV 4 world->shadow-UV-depth matrices, one per cascade.
	 * @param cascadeSplits   Camera-space far distance of cascades 0..2.
	 */
	void setShadowMap(TextureSRV shadowMap, const Mat4 worldToShadowUV[4], const Vec4& cascadeSplits);

	/**
	 * @brief Draw the M1 test grid (animated cubes with amplification/mesh shaders).
	 *
	 * Renders into the currently bound render target + depth buffer of the
	 * renderer (it must be the MSAA HDR target the scene pass renders to).
	 * The pipeline is created lazily on the first call.
	 * @param view      World->view matrix.
	 * @param proj      View->clip matrix.
	 * @param timeSec   Time in seconds (drives the cube animation).
	 */
	Result<void, RenderError> drawGrid(const Mat4& view, const Mat4& proj, F32 timeSec);

protected:
	Result<void, CoreError> onInitialize() override;
	void onShutdown() override;

private:
	struct Impl;
	Result<void, RenderError> ensurePipeline(Impl& p, UInt32 sampleCount); ///< Lazy pipeline + buffer creation.
	Result<void, RenderError> ensureScenePipeline(Impl& p, UInt32 sampleCount);
	Result<void, RenderError> ensureClusterPipeline(Impl& p, UInt32 sampleCount);
	///< Shared implementation of drawScene / drawSceneGBuffer.
	Result<void, RenderError> drawSceneImpl(const Vector<MeshDrawGroup>& groups, const Mat4& view, const Mat4& proj, F32 timeSec, bool gbuffer);
	///< Wraps an instance list into a single group covering every registered mesh.
	EE_NODISCARD Vector<MeshDrawGroup> singleGroup(const Vector<Mat4>& instanceMatrices) const;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
