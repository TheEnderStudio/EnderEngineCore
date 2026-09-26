#pragma once

#include <Engine/Core/Pipeline.hpp>
#include <Engine/Core/Types.hpp>
#include <Engine/Jobs/JobExecutor.hpp>

#include <Engine/Rendering/RenderTypes.hpp>
#include <Engine/Rendering/RenderSubsystem.hpp>
#include <Engine/Rendering/ShadowSubsystem.hpp>
#include <Engine/Rendering/MeshShaderSubsystem.hpp>
#include <Engine/Rendering/RayTracingSubsystem.hpp>
#include <Engine/Rendering/DenoisingSubsystem.hpp>
#include <Engine/PostProcess/PostProcessSubsystem.hpp>
#include <Engine/Rendering/DebugUISubsystem.hpp>
#include <Engine/Rendering/ComputeSubsystem.hpp>
#include <Engine/Rendering/Errors.hpp>
#include <Engine/UI/UISubsystem.hpp>

#include <functional>

EE_NAMESPACE_RENDERING_BEGIN

/// @brief Names of the passes RenderPipeline registers, in graph order.
///
/// Exposed so callers can gate passes without holding task ids
/// (see RenderPipeline::setPassEnabled).
namespace RenderPass {
	inline constexpr const char* Shadow = "shadow";          ///< Cascaded shadow map (raster only).
	inline constexpr const char* RtScene = "rtScene";        ///< BLAS/TLAS update (hybrid only).
	inline constexpr const char* Scene = "scene";            ///< Shaded scene into the HDR target (raster).
	inline constexpr const char* GBuffer = "gbuffer";        ///< G-buffer pass (hybrid).
	inline constexpr const char* Resolve = "resolve";        ///< MSAA G-buffer resolve (hybrid, MSAA only).
	inline constexpr const char* Trace = "trace";            ///< Ray trace (hybrid).
	inline constexpr const char* Denoise = "denoise";        ///< NRD / OIDN / temporal (hybrid).
	inline constexpr const char* Compose = "compose";        ///< RT compose into the HDR target (hybrid).
	inline constexpr const char* UiPost = "uiPost";          ///< UI layers that receive post-processing.
	inline constexpr const char* PostExecute = "postExecute";///< Post-processing chain.
	inline constexpr const char* UiLate = "uiLate";          ///< UI layers drawn after post-processing.
	inline constexpr const char* DebugUi = "debugUi";        ///< Debug UI render.
	inline constexpr const char* Present = "present";        ///< Present the swap chain.
}

/**
 * @brief One drawable piece of scene geometry.
 *
 * The same list feeds the colour pass and the shadow pass, so a mesh can never
 * be visible but unshadowed (or vice versa).
 */
struct SceneDraw {
	/// @brief How the geometry is submitted.
	enum class Kind {
		Single,    ///< One mesh with one transform.
		Instanced, ///< One mesh with a matrix per instance.
		Indirect,  ///< One mesh drawn with GPU-built indirect arguments.
	};

	Kind kind = Kind::Single;
	MeshHandle mesh;

	/// Used by Kind::Single.
	Transform transform;

	/// Used by Kind::Instanced.
	Vector<Mat4> matrices;

	/// Used by Kind::Indirect (buffers owned by the caller, typically ComputeSubsystem).
	ComputeSRV worldMatricesSRV;
	ComputeSRV indicesSRV;
	ComputeBuf indirectArgs;
	UInt32 argsByteOffset = 0;
};

/**
 * @brief Everything RenderPipeline needs to know about one frame.
 *
 * The pipeline owns pass ordering and resource bracketing; the application owns
 * the content, so it fills this in before render(). Fields under "derived" are
 * written by the pipeline's own tasks and should not be set by the caller.
 */
struct RenderFrame {
	// ------------------------------------------------------------------
	// Timing and target size
	// ------------------------------------------------------------------
	F64 deltaTime = 0.0;          ///< Seconds since the previous frame.
	F32 timeSec = 0.0;            ///< Free-running time, forwarded to the mesh shader path.
	UInt32 width = 0;             ///< Render target width in pixels.
	UInt32 height = 0;            ///< Render target height in pixels.

	// ------------------------------------------------------------------
	// Camera
	// ------------------------------------------------------------------
	Mat4 view = Mat4(1.0f);       ///< World -> view.
	Mat4 proj = Mat4(1.0f);       ///< View -> clip.
	Vec3 cameraPos = Vec3(0.0f);  ///< World-space camera position.

	// ------------------------------------------------------------------
	// Mode
	// ------------------------------------------------------------------
	/// true = hybrid ray tracing (G-buffer + trace + compose), false = raster.
	bool hybrid = false;
	/// Run the denoiser on the traced output (hybrid only).
	bool denoise = true;
	/// Denoiser selection: 0 = auto (NRD when ready, else OIDN/temporal),
	/// 1 = NRD, 2 = OIDN GPU, 3 = temporal/spatial.
	int denoiserSelection = 0;
	/// Denoiser history weight passed to NRD / the temporal filter.
	F32 denoiseStrength = 0.9f;

	// ------------------------------------------------------------------
	// Scene geometry
	// ------------------------------------------------------------------
	/// When true the mesh shader cluster-LOD path draws the whole scene with a
	/// single DrawMesh (meshGroups); otherwise sceneDraws is used.
	bool useMeshShaderScene = false;
	/// Draw groups for the mesh shader path (one per object type).
	Vector<MeshShaderSubsystem::MeshDrawGroup> meshGroups;
	/// Classic geometry list, also used for the shadow pass.
	Vector<SceneDraw> sceneDraws;
	/// Billboard batches drawn at the end of the raster scene pass.
	Vector<RenderSubsystem::BillboardDesc> billboards;

	/**
	 * @brief Optional extra raster work, run inside the scene pass.
	 *
	 * Invoked after the scene geometry and before the billboards, while the
	 * HDR target and depth buffer are still bound - the only point at which
	 * application-specific geometry can be issued without breaking the pass
	 * bracketing. Leave null when there is nothing extra to draw.
	 */
	std::function<Result<void, CoreError>(RenderSubsystem&)> sceneExtras;

	// ------------------------------------------------------------------
	// Shadows (raster only; the hybrid path ray traces its shadows)
	// ------------------------------------------------------------------
	Vec3 shadowLightDir = Vec3(0.0f, -1.0f, 0.0f); ///< Direction the light travels.
	Vec3 shadowEye = Vec3(0.0f);                   ///< Cascade fitting: camera position.
	Vec3 shadowCenter = Vec3(0.0f);                ///< Cascade fitting: look-at point.
	Vec3 shadowUp = Vec3(0.0f, 1.0f, 0.0f);        ///< Cascade fitting: up vector.
	F32 shadowFov = 1.0f;                          ///< Cascade fitting: vertical FOV (radians).
	F32 shadowAspect = 1.0f;                       ///< Cascade fitting: aspect ratio.
	F32 shadowNear = 0.01f;                        ///< Cascade fitting: near plane.
	F32 shadowFar = 100.0f;                        ///< Cascade fitting: far plane.

	// ------------------------------------------------------------------
	// Hybrid ray tracing
	// ------------------------------------------------------------------
	/// Objects to place in the RT scene (BLAS/TLAS).
	Vector<RayTracedObjectGroup> rtGroups;
	/// Per-frame trace constants (inverse view-projection, light, quality knobs).
	RayTraceConstants rtConstants;
	/// Ray traced output resolution (may be lower than the render target).
	UInt32 rtWidth = 0;
	UInt32 rtHeight = 0;
	/// G-buffer targets written by the gbuffer pass.
	TextureHandle gBufferColor, gBufferNormal, gBufferEmissive, gBufferDepth;
	/// Single-sample resolve targets (used when the G-buffer is MSAA).
	TextureHandle resolveColor, resolveNormal, resolveEmissive, resolveDepth;
	/// Ray traced output plus the filter guides.
	TextureHandle rtTex, rtAlbedo, rtNormal;
	/// Debug draw mode forwarded to compose(): 0 = shaded, 1..5 = G-buffer views.
	UInt32 rtDrawMode = 0;
	/// Light colour used by the compose pass.
	Vec3 lightColor = Vec3(1.0f);

	// ------------------------------------------------------------------
	// Derived (written by the pipeline's tasks; do not set)
	// ------------------------------------------------------------------
	TextureHandle sourceColor, sourceNormal, sourceEmissive, sourceDepth; ///< G-buffer or its resolve.
	void* composeSRV = nullptr;                                          ///< Traced/denoised SRV for compose.

	/// @brief Reset the derived fields to their pre-pass state.
	void resetDerived() {
		sourceColor = gBufferColor;
		sourceNormal = gBufferNormal;
		sourceEmissive = gBufferEmissive;
		sourceDepth = gBufferDepth;
		composeSRV = nullptr;
	}
};

/**
 * @brief The rendering subsystems RenderPipeline drives.
 *
 * All pointers except @c compute are required and must outlive the pipeline.
 * The pipeline only calls into a subsystem while it is in the Running state, so
 * a subsystem that was never initialized simply causes its pass to be skipped.
 */
struct RenderPipelineContext {
	RenderSubsystem* renderer = nullptr;      ///< Required.
	ShadowSubsystem* shadow = nullptr;        ///< Required.
	MeshShaderSubsystem* meshShader = nullptr;///< Required.
	RayTracingSubsystem* rayTracing = nullptr;///< Required.
	DenoisingSubsystem* denoising = nullptr;  ///< Required.
	PostProcess::PostProcessSubsystem* postProcess = nullptr; ///< Required.
	UI::UISubsystem* ui = nullptr;            ///< Required.
	DebugUISubsystem* debugUI = nullptr;      ///< Required.

	/// @return true if every required subsystem is set.
	EE_NODISCARD bool isComplete() const {
		return renderer && shadow && meshShader && rayTracing && denoising
			&& postProcess && ui && debugUI;
	}
};

/**
 * @brief The engine's rendering pass graph.
 *
 * RenderPipeline is a @ref Pipeline (PipelineBase<Jobs::JobExecutor>) whose tasks
 * are the rendering passes. It owns pass ordering and the render-target
 * bracketing around each pass; the application owns frame content and supplies
 * it through @ref RenderFrame.
 *
 * The graph is fixed and validated once in build(); per-frame variation is
 * expressed by enabling and disabling passes:
 * - @c shadow runs only in raster mode. The hybrid path takes its shadows from
 *   the ray trace, and neither the engine's nor the mesh shader's G-buffer
 *   pixel shader samples the shadow map, so the four-cascade shadow pass is
 *   dead work there.
 * - @c rtScene / @c gbuffer / @c resolve / @c trace / @c denoise / @c compose
 *   run only in hybrid mode.
 * - @c resolve additionally requires more than one MSAA sample.
 *
 * Dependencies:
 * @verbatim
 *   shadow ──> scene ─────────────────────────────┐
 *   gbuffer ──> resolve ──┐                       │
 *   rtScene ──────────────┴─> trace ──> denoise ──> compose ──┐
 *                                                             ├─> uiPost ─> postExecute ─> uiLate ─> debugUi ─> present
 *   scene ────────────────────────────────────────────────────┘
 * @endverbatim
 *
 * All passes run as main-thread tasks. That is not a limitation of the
 * Pipeline: every pass submits work to the same D3D12 immediate context, which
 * is not thread safe. @c PipelineTaskMode::Parallel is available for
 * application-added tasks that touch no rendering subsystem.
 */
/**
 * @brief GPU time of one render pass (duration query, read one frame late).
 */
struct RenderPassGpuTime {
	F64 lastMs = 0.0;    ///< GPU milliseconds of the last measured frame.
	F64 averageMs = 0.0; ///< Exponential moving average of lastMs.
	F64 maxMs = 0.0;     ///< Largest lastMs observed (catches intermittent hitches).
};

class EE_API RenderPipeline : public Pipeline {
public:
	/**
	 * @brief Construct the pipeline over a set of rendering subsystems.
	 * @param executor The job executor backing the pipeline.
	 * @param context The subsystems to drive. Must stay alive and valid.
	 * @param name Pipeline name (used for logging and profiling).
	 */
	RenderPipeline(Jobs::JobExecutor& executor, const RenderPipelineContext& context, StringView name = "RenderPipeline");

	~RenderPipeline() override;

	EE_NO_COPY(RenderPipeline)

	/// @return The subsystems this pipeline drives.
	EE_NODISCARD const RenderPipelineContext& context() const;

	/**
	 * @brief Configure the pass graph for one frame and run it.
	 *
	 * Enables or disables passes according to the frame's mode, then executes
	 * the graph. @p frame is stored as the run payload and its derived fields
	 * are updated by the passes.
	 * @param frame The frame description; must outlive the call.
	 * @return Result indicating success, or the first failing pass's error.
	 */
	Result<void, CoreError> render(RenderFrame& frame);

	/**
	 * @brief Enable or disable a pass for the next frame.
	 *
	 * The per-frame mode setup in render() overwrites the mode-driven passes
	 * (shadow, gbuffer, trace, ...), so this is intended for the always-on tail
	 * (uiPost, postExecute, uiLate, debugUi, present) and for debugging.
	 * @param passName One of the RenderPass names.
	 * @param enabled false to skip the pass.
	 * @return Result indicating success, or InvalidArgument for an unknown name.
	 */
	Result<void, CoreError> setPassEnabled(StringView passName, bool enabled);

	/// @return Whether the named pass exists (regardless of its enabled state).
	EE_NODISCARD bool hasPass(StringView passName) const;

	/// @return Id of the named pass, or an invalid id.
	EE_NODISCARD PipelineTaskId passId(StringView passName) const;

	/// @brief Per-pass statistics of the most recent frame (same order as describe()).
	///
	/// The reported time is CPU time: it covers what the pass spends *recording*
	/// its commands, which for a GPU pass is not its cost. All the work of a frame
	/// is submitted and waited on once, in the present pass, so that pass appears
	/// to take as long as the whole GPU frame. Use passGpuStats() to see the GPU
	/// side.
	EE_NODISCARD const Vector<PipelineTaskStats>& passStats() const;

	/**
	 * @brief GPU time of each pass, measured with a duration query (same order as passStats()).
	 *
	 * The values are read back one frame late and never stall the pipeline, and
	 * the present pass is excluded because its begin/end timestamps would land on
	 * either side of the frame submission (its "GPU cost" is the wait for the
	 * frame, which already shows up as its CPU time).
	 */
	EE_NODISCARD const Vector<RenderPassGpuTime>& passGpuStats() const;

	/// @brief Enable/disable the per-pass GPU timers (see passGpuStats()).
	void setGpuTiming(bool enabled);
	EE_NODISCARD bool gpuTiming() const;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_RENDERING_END
