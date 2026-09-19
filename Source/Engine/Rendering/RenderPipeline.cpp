#include <Engine/Rendering/RenderPipeline.hpp>
#include <Engine/Core/Log.hpp>

#include <algorithm>

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
	addTask(RenderPass::Shadow, [this, c](const PipelineFrameContext& context) -> Result<void, CoreError> {
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
	}, PipelineTaskMode::MainThread, c->shadow);

	// ------------------------------------------------------------------
	// rtScene - BLAS/TLAS update. Independent of every raster pass, so it shares
	// the first level with the shadow and G-buffer passes.
	// ------------------------------------------------------------------
	addTask(RenderPass::RtScene, [c](const PipelineFrameContext& context) -> Result<void, CoreError> {
		RenderFrame& frame = *static_cast<RenderFrame*>(context.payload);
		if (frame.rtGroups.empty()) return {};
		const Result<void, RenderError> result = c->rayTracing->updateScene(frame.rtGroups);
		if (result.isErr()) {
			EError("RenderPipeline: RT updateScene failed: {}", ToString(result.error()));
			return toCoreError(result.error());
		}
		return {};
	}, PipelineTaskMode::MainThread, c->rayTracing);

	// ------------------------------------------------------------------
	// scene - shaded pass into the HDR target (raster).
	// ------------------------------------------------------------------
	PipelineTaskId scene = addTask(RenderPass::Scene, [this, c](const PipelineFrameContext& context) -> Result<void, CoreError> {
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
	}, PipelineTaskMode::MainThread, c->renderer);
	impl->link(scene, passId(RenderPass::Shadow));

	// ------------------------------------------------------------------
	// gbuffer - hybrid G-buffer pass.
	// ------------------------------------------------------------------
	PipelineTaskId gbuffer = addTask(RenderPass::GBuffer, [this, c](const PipelineFrameContext& context) -> Result<void, CoreError> {
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
	}, PipelineTaskMode::MainThread, c->renderer);

	// ------------------------------------------------------------------
	// resolve - MSAA G-buffer down to single-sample targets, which the trace and
	// compose compute shaders read. Skipped at 1x.
	// ------------------------------------------------------------------
	PipelineTaskId resolve = addTask(RenderPass::Resolve, [c](const PipelineFrameContext& context) -> Result<void, CoreError> {
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
	}, PipelineTaskMode::MainThread, c->renderer);
	impl->link(resolve, gbuffer);

	// ------------------------------------------------------------------
	// trace - shadow + reflection rays against the G-buffer.
	// ------------------------------------------------------------------
	PipelineTaskId trace = addTask(RenderPass::Trace, [c](const PipelineFrameContext& context) -> Result<void, CoreError> {
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
	}, PipelineTaskMode::MainThread, c->rayTracing);
	impl->link(trace, { resolve, passId(RenderPass::RtScene) });

	// ------------------------------------------------------------------
	// denoise - NRD, else OIDN / temporal.
	// ------------------------------------------------------------------
	PipelineTaskId denoise = addTask(RenderPass::Denoise, [c](const PipelineFrameContext& context) -> Result<void, CoreError> {
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
	}, PipelineTaskMode::MainThread, c->rayTracing);
	impl->link(denoise, trace);

	// ------------------------------------------------------------------
	// compose - blend the traced result over the skybox into the HDR target.
	// ------------------------------------------------------------------
	PipelineTaskId compose = addTask(RenderPass::Compose, [c](const PipelineFrameContext& context) -> Result<void, CoreError> {
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
	}, PipelineTaskMode::MainThread, c->rayTracing);
	impl->link(compose, denoise);

	// ------------------------------------------------------------------
	// Shared tail. uiPost waits for the scene and for compose, but either may be
	// disabled by the frame mode; a skipped dependency is not a failure, so the
	// tail always runs.
	// ------------------------------------------------------------------
	PipelineTaskId uiPost = addTask(RenderPass::UiPost, [c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->ui->beginFrame(false);
		c->ui->endFrame();
		return {};
	}, PipelineTaskMode::MainThread, c->ui);
	impl->link(uiPost, { scene, compose });

	PipelineTaskId postExecute = addTask(RenderPass::PostExecute, [c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->postProcess->execute();
		return {};
	}, PipelineTaskMode::MainThread, c->postProcess);
	impl->link(postExecute, uiPost);

	PipelineTaskId uiLate = addTask(RenderPass::UiLate, [c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->ui->beginFrame(true);
		c->ui->endFrame();
		return {};
	}, PipelineTaskMode::MainThread, c->ui);
	impl->link(uiLate, postExecute);

	// The application calls debugUI.beginFrame() earlier in the frame to build
	// the UI; this pass only renders it.
	PipelineTaskId debugUi = addTask(RenderPass::DebugUi, [c](const PipelineFrameContext&) -> Result<void, CoreError> {
		c->debugUI->endFrameAndRender();
		return {};
	}, PipelineTaskMode::MainThread, c->debugUI);
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
	return run(context);
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

EE_NAMESPACE_RENDERING_END
