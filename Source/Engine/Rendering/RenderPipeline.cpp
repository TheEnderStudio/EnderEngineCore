#include <Engine/Rendering/RenderPipeline.hpp>
#include <Engine/Core/Log.hpp>

#include <DiligentCore/Graphics/GraphicsEngine/interface/RenderDevice.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/DeviceContext.h>
#include <DiligentCore/Graphics/GraphicsEngine/interface/Query.h>
#include <DiligentCore/Common/interface/RefCntAutoPtr.hpp>

#include <algorithm>

namespace D = Diligent;

EE_NAMESPACE_RENDERING_BEGIN

namespace {

/// @brief Map a RenderError onto the pipeline's error space.
///
/// The pipeline speaks CoreError; the precise render error is logged at the
/// failing pass before it is mapped.
inline CoreError toCoreError(RenderError) {
	return CoreError::OperationFailed;
}

/// @brief Run a render call and log+map its failure.
template <typename TCallable>
CoreError runPass(RenderPipeline& pipeline, const char* passName, TCallable&& call) {
	const Result<void, RenderError> result = call();
	if (result.isErr()) {
		EError("RenderPipeline '{}': pass '{}' failed: {}",
			pipeline.name(), passName, ToString(result.error()));
		return toCoreError(result.error());
	}
	return CoreError::None;
}

} // namespace

/**
 * @brief Implementation state of RenderPipeline.
 */
struct RenderPipeline::Impl {
	RenderPipelineContext context;
	UInt64 frameIndex = 0;
	F64 elapsedTime = 0.0;

	// ------------------------------------------------------------------
	// GPU timing.
	//
	// The per-pass times in PipelineTaskStats are CPU times: a pass only
	// *records* its commands, so its cost is invisible there. Everything is
	// submitted and waited on once, inside the present pass (Flush +
	// WaitForFrame + vsync), which is why that pass looks as expensive as the
	// whole GPU frame. These duration queries time the GPU work between passes
	// so the table can show where the time actually goes. They are read a frame
	// late and never block.
	//
	// The present pass is deliberately not wrapped: its begin/end timestamps
	// would fall on either side of the frame submission.
	// ------------------------------------------------------------------
	Vector<D::RefCntAutoPtr<D::IQuery>> gpuQueries; ///< One duration query per pass (present excluded).
	Vector<RenderPassGpuTime> gpuStats;             ///< Results, indexed like passStats().
	Vector<UInt8> gpuEnded;                         ///< Per slot: the query has been ended at least once.
	Size nextGpuSlot = 0;                           ///< Registration order -> task id.
	D::IDeviceContext* gpuCtx = nullptr;
	bool gpuTiming = true;

	/// @brief Wrap one pass so its GPU work is timed.
	///
	/// Must be called exactly once per addTask(), in registration order: the slot
	/// is the index the pipeline assigns to the pass.
	PipelineTaskFn timed(PipelineTaskFn fn) {
		const Size slot = nextGpuSlot++;
		if (gpuStats.size() <= slot) {
			gpuStats.resize(slot + 1);
			gpuEnded.resize(slot + 1, 0);
		}
		return [this, slot, fn = std::move(fn)](const PipelineFrameContext& frame) -> Result<void, CoreError> {
			if (!gpuTiming || !gpuCtx || slot >= gpuQueries.size() || !gpuQueries[slot])
				return fn(frame);
			D::IQuery* query = gpuQueries[slot];

			// Read the *previous* frame's measurement here - before BeginQuery,
			// not after EndQuery. Two reasons, both of which silently produced a
			// column of zeros when this was written the other way round:
			//   * Diligent re-allocates the query's query-heap slots inside
			//     BeginQuery, so the old timestamps are gone once a new frame has
			//     started;
			//   * the fence the measurement is compared against is only reached
			//     once the frame that produced it has been submitted *and*
			//     waited on - which happens in the present pass, i.e. before the
			//     next frame's passes run.
			// GetData is a non-blocking poll: it returns false while the frame is
			// still in flight, in which case the previous value is kept.
			if (gpuEnded[slot]) {
				D::QueryDataDuration data;
				if (query->GetData(&data, sizeof(data), false) && data.Frequency != 0) {
					const F64 ms = (F64)data.Duration / (F64)data.Frequency * 1000.0;
					RenderPassGpuTime& t = gpuStats[slot];
					t.lastMs = ms;
					t.maxMs = (std::max)(t.maxMs, ms);
					t.averageMs = (t.averageMs == 0.0) ? ms : (t.averageMs * 0.9 + ms * 0.1);
				}
			}

			gpuCtx->BeginQuery(query);
			const Result<void, CoreError> result = fn(frame);
			gpuCtx->EndQuery(query);
			gpuEnded[slot] = 1;
			return result;
		};
	}

	/// @brief Create the query objects (once) and remember the context to submit them on.
	void prepareGpuTiming(D::IRenderDevice* device, D::IDeviceContext* ctx) {
		gpuCtx = ctx;
		if (!device || !ctx) { gpuTiming = false; return; }
		if (gpuQueries.size() != nextGpuSlot) gpuQueries.resize(nextGpuSlot);
		for (Size i = 0; i < gpuQueries.size(); ++i) {
			if (gpuQueries[i]) continue;
			D::QueryDesc desc;
			desc.Name = "RenderPipeline pass timer";
			desc.Type = D::QUERY_TYPE_DURATION;
			device->CreateQuery(desc, &gpuQueries[i]);
			if (!gpuQueries[i]) {
				EError("RenderPipeline '{}': duration queries unavailable - per-pass GPU times are disabled.", m_pipeline->name());
				gpuTiming = false;
				gpuQueries.clear();
				return;
			}
		}
		EInfo("RenderPipeline '{}': per-pass GPU timers enabled ({} duration queries).",
			m_pipeline->name(), gpuQueries.size());
	}

	/// @return Whether any pass produced a GPU measurement so far.
	bool anyGpuMeasured() const {
		for (const RenderPassGpuTime& t : gpuStats)
			if (t.averageMs > 0.0) return true;
		return false;
	}

	/// @brief Register a dependency, logging if the ids are somehow invalid.
	void link(PipelineTaskId dependent, PipelineTaskId dependency) {
		const Result<void, CoreError> result = m_pipeline->dependsOn(dependent, dependency);
		if (result.isErr()) {
			EError("RenderPipeline '{}': failed to link passes ({} -> {}).",
				m_pipeline->name(), dependent.index, dependency.index);
		}
	}

	void link(PipelineTaskId dependent, const Vector<PipelineTaskId>& dependencies) {
		const Result<void, CoreError> result = m_pipeline->dependsOn(dependent, dependencies);
		if (result.isErr()) {
			EError("RenderPipeline '{}': failed to link passes ({}).", m_pipeline->name(), dependent.index);
		}
	}

	Pipeline* m_pipeline = nullptr;
};

RenderPipeline::RenderPipeline(Jobs::JobExecutor& executor, const RenderPipelineContext& context, StringView name)
	: Pipeline(executor, name), m_impl(std::make_unique<Impl>()) {
	m_impl->context = context;
	m_impl->m_pipeline = this;

	if (!context.isComplete()) {
		EError("RenderPipeline '{}': every rendering subsystem must be provided.", name);
		return;
	}

	RenderPipelineContext* const c = &m_impl->context;
	Impl* const impl = m_impl.get();

	// ------------------------------------------------------------------
	// shadow - cascaded shadow map. Raster only: the hybrid path takes its
	// shadows from the ray trace, and no G-buffer shader samples the shadow map.
	// ------------------------------------------------------------------
	addTask(RenderPass::Shadow, impl->timed([this, c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		ShadowSubsystem& shadow = *c->shadow;
		RenderSubsystem& renderer = *c->renderer;

		shadow.distribute(frame.shadowLightDir, frame.shadowEye, frame.shadowCenter, frame.shadowUp,
			frame.shadowFov, frame.shadowAspect, frame.shadowNear, frame.shadowFar);
		renderer.clearShadowCascades(shadow);

		// The shadow pass walks the same geometry list as the colour pass, so a
		// mesh can never be visible but unshadowed.
		for (const SceneDraw& draw : frame.sceneDraws) {
			switch (draw.kind) {
			case SceneDraw::Kind::Single:
				renderer.renderShadowPass(shadow, draw.mesh, draw.transform.computeWorldMatrix());
				break;
			case SceneDraw::Kind::Instanced:
				for (const Mat4& world : draw.matrices) renderer.renderShadowPass(shadow, draw.mesh, world);
				break;
			case SceneDraw::Kind::Indirect:
				renderer.renderShadowPassIndirect(shadow, draw.mesh, draw.worldMatricesSRV,
					draw.indicesSRV, draw.indirectArgs, draw.argsByteOffset);
				break;
			}
		}

		renderer.setShadowSRV(shadow.getSRV());
		Mat4 cascadeUV[4];
		for (UInt32 i = 0; i < 4; ++i) cascadeUV[i] = shadow.getWorldToShadowMapUVDepth(i);
		const Vec4 splits = shadow.getCascadeSplitDistances();
		renderer.setShadowData(cascadeUV, splits);
		// The mesh shader's pixel shader uses the forward lighting model, so it
		// needs the same cascades.
		c->meshShader->setShadowMap(renderer.getShadowSRV(), cascadeUV, splits);
		return {};
	}), PipelineTaskMode::MainThread, c->shadow);

	// ------------------------------------------------------------------
	// rtScene - BLAS/TLAS update. Independent of every raster pass, so it shares
	// the first level with the shadow and G-buffer passes.
	// ------------------------------------------------------------------
	addTask(RenderPass::RtScene, impl->timed([c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		if (frame.rtGroups.empty()) return {};
		const Result<void, RenderError> result = c->rayTracing->updateScene(frame.rtGroups);
		if (result.isErr()) {
			EError("RenderPipeline: RT updateScene failed: {}", ToString(result.error()));
			return toCoreError(result.error());
		}
		return {};
	}), PipelineTaskMode::MainThread, c->rayTracing);

	// ------------------------------------------------------------------
	// scene - shaded pass into the HDR target (raster).
	// ------------------------------------------------------------------
	PipelineTaskId scene = addTask(RenderPass::Scene, impl->timed([this, c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		RenderSubsystem& renderer = *c->renderer;

		renderer.setRenderTarget(c->postProcess->getHDRRTV());
		renderer.beginFrame();

		if (frame.useMeshShaderScene) {
			const CoreError error = runPass(*this, RenderPass::Scene, [&] {
				return c->meshShader->drawScene(frame.meshGroups, frame.view, frame.proj, frame.timeSec);
			});
			if (error != CoreError::None) return error;
		}
		else {
			for (const SceneDraw& draw : frame.sceneDraws) {
				switch (draw.kind) {
				case SceneDraw::Kind::Single:
					renderer.drawMesh(draw.mesh, draw.transform);
					break;
				case SceneDraw::Kind::Instanced:
					renderer.drawMeshInstanced(draw.mesh, draw.matrices);
					break;
				case SceneDraw::Kind::Indirect:
					renderer.drawMeshInstancedIndirect(draw.mesh, draw.worldMatricesSRV,
						draw.indicesSRV, draw.indirectArgs, draw.argsByteOffset);
					break;
				}
			}
		}

		// Application-specific raster work, still inside the pass bracket.
		if (frame.sceneExtras) {
			const Result<void, CoreError> extra = frame.sceneExtras(renderer);
			if (extra.isErr()) return extra.error();
		}

		if (!frame.billboards.empty()) renderer.drawBillboards(frame.billboards);

		renderer.endFrame();
		renderer.setRenderTarget(nullptr);
		return {};
	}), PipelineTaskMode::MainThread, c->renderer);
	impl->link(scene, passId(RenderPass::Shadow));

	// ------------------------------------------------------------------
	// gbuffer - hybrid G-buffer pass.
	// ------------------------------------------------------------------
	PipelineTaskId gbuffer = addTask(RenderPass::GBuffer, impl->timed([this, c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		RenderSubsystem& renderer = *c->renderer;

		renderer.beginGBuffer(renderer.getTextureRTV(frame.gBufferColor),
			renderer.getTextureRTV(frame.gBufferNormal),
			renderer.getTextureRTV(frame.gBufferEmissive),
			renderer.getTextureDSV(frame.gBufferDepth));

		if (frame.useMeshShaderScene) {
			const CoreError error = runPass(*this, RenderPass::GBuffer, [&] {
				return c->meshShader->drawSceneGBuffer(frame.meshGroups, frame.view, frame.proj, frame.timeSec);
			});
			if (error != CoreError::None) return error;
		}
		else {
			for (const SceneDraw& draw : frame.sceneDraws) {
				switch (draw.kind) {
				case SceneDraw::Kind::Single:
					renderer.drawMesh(draw.mesh, draw.transform);
					break;
				case SceneDraw::Kind::Instanced:
					renderer.drawMeshInstanced(draw.mesh, draw.matrices);
					break;
				case SceneDraw::Kind::Indirect:
					renderer.drawMeshInstancedIndirect(draw.mesh, draw.worldMatricesSRV,
						draw.indicesSRV, draw.indirectArgs, draw.argsByteOffset);
					break;
				}
			}
		}

		renderer.endGBuffer();
		// Default guide textures are the G-buffer itself; the resolve pass
		// replaces them when it runs.
		frame.sourceColor = frame.gBufferColor;
		frame.sourceNormal = frame.gBufferNormal;
		frame.sourceEmissive = frame.gBufferEmissive;
		frame.sourceDepth = frame.gBufferDepth;
		return {};
	}), PipelineTaskMode::MainThread, c->renderer);

	// ------------------------------------------------------------------
	// resolve - MSAA G-buffer down to single-sample targets, which the trace and
	// compose compute shaders read. Skipped at 1x.
	// ------------------------------------------------------------------
	PipelineTaskId resolve = addTask(RenderPass::Resolve, impl->timed([c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		RenderSubsystem& renderer = *c->renderer;

		const Result<void, RenderError> result = renderer.resolveGBufferMSAA(
			renderer.getTextureSRV(frame.gBufferColor), renderer.getTextureSRV(frame.gBufferNormal),
			renderer.getTextureSRV(frame.gBufferEmissive), renderer.getTextureSRV(frame.gBufferDepth),
			renderer.getTextureUAV(frame.resolveColor), renderer.getTextureUAV(frame.resolveNormal),
			renderer.getTextureUAV(frame.resolveEmissive), renderer.getTextureUAV(frame.resolveDepth),
			frame.width, frame.height);
		if (result.isErr()) {
			EError("RenderPipeline: G-buffer MSAA resolve failed: {}", ToString(result.error()));
			return toCoreError(result.error());
		}
		frame.sourceColor = frame.resolveColor;
		frame.sourceNormal = frame.resolveNormal;
		frame.sourceEmissive = frame.resolveEmissive;
		frame.sourceDepth = frame.resolveDepth;
		return {};
	}), PipelineTaskMode::MainThread, c->renderer);
	impl->link(resolve, gbuffer);

	// ------------------------------------------------------------------
	// trace - shadow + reflection rays against the G-buffer.
	// ------------------------------------------------------------------
	PipelineTaskId trace = addTask(RenderPass::Trace, impl->timed([c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		RenderSubsystem& renderer = *c->renderer;

		const Result<void, RenderError> result = c->rayTracing->trace(frame.rtConstants,
			renderer.getTextureSRV(frame.sourceNormal), renderer.getTextureSRV(frame.sourceDepth),
			renderer.getTextureSRV(frame.sourceColor),
			renderer.getTextureUAV(frame.rtTex), renderer.getTextureUAV(frame.rtAlbedo),
			renderer.getTextureUAV(frame.rtNormal), frame.rtWidth, frame.rtHeight);
		if (result.isErr()) {
			EError("RenderPipeline: RT trace failed: {}", ToString(result.error()));
			return toCoreError(result.error());
		}
		return {};
	}), PipelineTaskMode::MainThread, c->rayTracing);
	impl->link(trace, { resolve, passId(RenderPass::RtScene) });

	// ------------------------------------------------------------------
	// denoise - NRD, else OIDN / temporal.
	// ------------------------------------------------------------------
	PipelineTaskId denoise = addTask(RenderPass::Denoise, impl->timed([c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		RenderSubsystem& renderer = *c->renderer;
		const Mat4 viewProj = frame.proj * frame.view;

		void* composeSRV = renderer.getTextureSRV(frame.rtTex);
		bool denoised = false;

		const bool wantNRD = (frame.denoiserSelection == 0 || frame.denoiserSelection == 1) && c->denoising->isReady();
		if (wantNRD) {
			c->denoising->setStrength(frame.denoiseStrength);
			const Result<void, RenderError> result = c->denoising->denoise(
				renderer.getTextureSRV(frame.rtTex), renderer.getTextureSRV(frame.sourceNormal),
				renderer.getTextureSRV(frame.sourceDepth), frame.view, frame.proj, frame.rtWidth, frame.rtHeight);
			if (result.isErr()) {
				EWarn("RenderPipeline: NRD denoise failed: {}", ToString(result.error()));
			}
			else if (void* denoisedSRV = c->denoising->getDenoisedSRV()) {
				composeSRV = denoisedSRV;
				denoised = true;
			}
		}

		if (!denoised) {
			c->rayTracing->setForceTemporal(frame.denoiserSelection == 3);
			c->rayTracing->setDenoiseStrength(frame.denoiseStrength);
			const Result<void, RenderError> result = c->rayTracing->denoise(
				renderer.getTextureSRV(frame.rtTex), renderer.getTextureSRV(frame.rtAlbedo),
				renderer.getTextureSRV(frame.rtNormal), renderer.getTextureSRV(frame.sourceDepth),
				glm::inverse(viewProj), viewProj, frame.rtWidth, frame.rtHeight);
			if (result.isErr()) {
				EWarn("RenderPipeline: denoise failed: {}", ToString(result.error()));
			}
			else if (void* denoisedSRV = c->rayTracing->getDenoisedSRV()) {
				composeSRV = denoisedSRV;
			}
		}

		frame.composeSRV = composeSRV;
		return {};
		// Gated on the RT subsystem, not on NRD: the pass falls back to the ray
		// tracer's own OIDN/temporal filter when NRD is unavailable, so NRD being
		// down must not disable denoising altogether.
	}), PipelineTaskMode::MainThread, c->rayTracing);
	impl->link(denoise, trace);

	// ------------------------------------------------------------------
	// compose - blend the traced result over the skybox into the HDR target.
	// ------------------------------------------------------------------
	PipelineTaskId compose = addTask(RenderPass::Compose, impl->timed([c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		RenderSubsystem& renderer = *c->renderer;

		renderer.setRenderTarget(c->postProcess->getHDRRTV());
		renderer.beginFrame();

		void* rtSRV = frame.composeSRV ? frame.composeSRV : renderer.getTextureSRV(frame.rtTex);
		const Mat4 viewProj = frame.proj * frame.view;

		const Result<void, RenderError> result = c->rayTracing->compose(
			renderer.getTextureSRV(frame.sourceColor), renderer.getTextureSRV(frame.sourceNormal),
			renderer.getTextureSRV(frame.sourceDepth), renderer.getTextureSRV(frame.sourceEmissive),
			rtSRV, c->postProcess->getHDRRTV(), renderer.getDepthStencil(), frame.rtDrawMode,
			glm::inverse(viewProj), frame.cameraPos, frame.lightColor, frame.width, frame.height);
		if (result.isErr()) {
			EError("RenderPipeline: RT compose failed: {}", ToString(result.error()));
			return toCoreError(result.error());
		}

		renderer.endFrame();
		renderer.setRenderTarget(nullptr);
		return {};
	}), PipelineTaskMode::MainThread, c->rayTracing);
	impl->link(compose, denoise);

	// ------------------------------------------------------------------
	// Shared tail. uiPost waits for the scene and for compose, but either may be
	// disabled by the frame mode; a skipped dependency is not a failure, so the
	// tail always runs.
	// ------------------------------------------------------------------
	PipelineTaskId uiPost = addTask(RenderPass::UiPost, impl->timed([c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->ui->beginFrame(false);
		c->ui->endFrame();
		return {};
	}), PipelineTaskMode::MainThread, c->ui);
	impl->link(uiPost, { scene, compose });

	PipelineTaskId postExecute = addTask(RenderPass::PostExecute, impl->timed([c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->postProcess->execute();
		return {};
	}), PipelineTaskMode::MainThread, c->postProcess);
	impl->link(postExecute, uiPost);

	PipelineTaskId uiLate = addTask(RenderPass::UiLate, impl->timed([c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->ui->beginFrame(true);
		c->ui->endFrame();
		return {};
	}), PipelineTaskMode::MainThread, c->ui);
	impl->link(uiLate, postExecute);

	// The application calls debugUI.beginFrame() earlier in the frame to build
	// the UI; this pass only renders it.
	PipelineTaskId debugUi = addTask(RenderPass::DebugUi, impl->timed([c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->debugUI->endFrameAndRender();
		return {};
	}), PipelineTaskMode::MainThread, c->debugUI);
	impl->link(debugUi, uiLate);

	PipelineTaskId present = addTask(RenderPass::Present, [c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->postProcess->present();
		return {};
	}, PipelineTaskMode::MainThread, c->postProcess);
	impl->link(present, debugUi);

	// Every pass is registered above, so the graph can be validated right away.
	// A subclass that adds passes must call build() again afterwards.
	const Result<void, CoreError> buildResult = build();
	if (buildResult.isErr()) {
		EError("RenderPipeline '{}': the pass graph is invalid: {}", name, ToString(buildResult.error()));
	}
}

RenderPipeline::~RenderPipeline() = default;

const RenderPipelineContext& RenderPipeline::context() const { return m_impl->context; }

Result<void, CoreError> RenderPipeline::render(RenderFrame& frame) {
	// The graph is fixed; the frame mode selects which passes apply. Disabling
	// is idempotent, so calling this every frame is safe.
	const bool hybrid = frame.hybrid;
	setTaskEnabled(passId(RenderPass::Shadow), !hybrid);
	setTaskEnabled(passId(RenderPass::RtScene), hybrid);
	setTaskEnabled(passId(RenderPass::Scene), !hybrid);
	setTaskEnabled(passId(RenderPass::GBuffer), hybrid);
	setTaskEnabled(passId(RenderPass::Resolve), hybrid && m_impl->context.renderer->msaaSamples() > 1);
	setTaskEnabled(passId(RenderPass::Trace), hybrid);
	setTaskEnabled(passId(RenderPass::Denoise), hybrid && frame.denoise);
	setTaskEnabled(passId(RenderPass::Compose), hybrid);

	frame.resetDerived();
	if (frame.rtWidth == 0) frame.rtWidth = frame.width;
	if (frame.rtHeight == 0) frame.rtHeight = frame.height;

	// Rebuild the prefiltered sky environment if it changed, before any pass can
	// sample it (it is recording-side work, so it has to happen inside the frame),
	// and hand the cube to the mesh shader path, which reads it for the specular
	// ambient in the passes that shade. Doing it here rather than in one of the
	// passes keeps it out of their enable/disable logic (the shadow pass, which
	// carries the other per-frame bindings, is off in the hybrid mode).
	m_impl->context.renderer->prepareEnvironment();
	m_impl->context.meshShader->setSkyEnv(m_impl->context.renderer->getSkyEnvSRV());

	// Keep the trace constants consistent with the camera the other passes use;
	// the remaining quality knobs are the caller's.
	const Mat4 viewProj = frame.proj * frame.view;
	frame.rtConstants.viewProjInv = glm::inverse(viewProj);
	frame.rtConstants.cameraPos = Vec4(frame.cameraPos, 1.0f);

	PipelineFrameContext context;
	context.frameIndex = m_impl->frameIndex++;
	context.deltaTime = frame.deltaTime;
	m_impl->elapsedTime += frame.deltaTime;
	context.elapsedTime = m_impl->elapsedTime;
	context.payload = &frame;

	// First frame: create the per-pass GPU timers on the device we are about to
	// submit to. Failure is not fatal - the table simply keeps showing CPU times.
	if (m_impl->gpuTiming && m_impl->gpuQueries.size() != m_impl->nextGpuSlot) {
		auto* device = static_cast<D::IRenderDevice*>(m_impl->context.renderer->getDevice());
		auto* ctx = static_cast<D::IDeviceContext*>(m_impl->context.renderer->getContext());
		m_impl->prepareGpuTiming(device, ctx);
	}

	const Result<void, CoreError> result = run(context);
	if (m_impl->gpuTiming && context.frameIndex == 240 && !m_impl->anyGpuMeasured()) {
		// The queries exist but never became readable: say so instead of leaving a
		// silent column of zeros in the statistics.
		EWarn("RenderPipeline '{}': per-pass GPU timers produced no data in {} frames (the query readback is not completing).",
			name(), context.frameIndex + 1);
	}
	return result;
}

Result<void, CoreError> RenderPipeline::setPassEnabled(StringView passName, bool enabled) {
	const Optional<PipelineTaskId> id = findTask(passName);
	if (!id.has_value()) {
		EError("RenderPipeline '{}': unknown pass '{}'.", name(), passName);
		return CoreError::InvalidArgument;
	}
	setTaskEnabled(id.value(), enabled);
	return {};
}

bool RenderPipeline::hasPass(StringView passName) const { return findTask(passName).has_value(); }

PipelineTaskId RenderPipeline::passId(StringView passName) const {
	const Optional<PipelineTaskId> id = findTask(passName);
	return id.has_value() ? id.value() : InvalidPipelineTaskId;
}

const Vector<PipelineTaskStats>& RenderPipeline::passStats() const { return taskStats(); }

const Vector<RenderPassGpuTime>& RenderPipeline::passGpuStats() const { return m_impl->gpuStats; }

void RenderPipeline::setGpuTiming(bool enabled) {
	m_impl->gpuTiming = enabled;
	// Clear the table either way: a stale column is worse than an empty one, and
	// after re-enabling the first measurement has to start a fresh Begin/End cycle.
	for (RenderPassGpuTime& t : m_impl->gpuStats) t = RenderPassGpuTime{};
	for (UInt8& ended : m_impl->gpuEnded) ended = 0;
}

bool RenderPipeline::gpuTiming() const { return m_impl->gpuTiming; }

EE_NAMESPACE_RENDERING_END
