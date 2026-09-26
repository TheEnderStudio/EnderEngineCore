#include <Engine/Rendering/RenderPipeline.hpp>
#include <Engine/Jobs/JobSubsystem.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace EnderEngine;
using namespace EnderEngine::Rendering;
using namespace EnderEngine::Jobs;

namespace {

/// @brief The eight rendering subsystems, none of them initialized.
///
/// A pass is only invoked while its subsystem is in the Running state, so an
/// uninitialized set makes the whole graph safely runnable without a GPU - which
/// is exactly what lets these tests check the graph and the gating logic.
struct RenderFixture {
	JobSubsystem jobs;
	JobExecutor executor{ jobs };

	RenderSubsystem renderer;
	ShadowSubsystem shadow;
	MeshShaderSubsystem meshShader;
	RayTracingSubsystem rayTracing;
	DenoisingSubsystem denoising;
	PostProcess::PostProcessSubsystem postProcess;
	UI::UISubsystem ui;
	DebugUISubsystem debugUI;

	RenderPipelineContext context;

	RenderFixture() {
		jobs.setWorkerThreadCount(2);
		jobs.initialize();

		context.renderer = &renderer;
		context.shadow = &shadow;
		context.meshShader = &meshShader;
		context.rayTracing = &rayTracing;
		context.denoising = &denoising;
		context.postProcess = &postProcess;
		context.ui = &ui;
		context.debugUI = &debugUI;
	}

	~RenderFixture() { jobs.shutdown(); }
};

/// @return A frame description with the minimum a run needs.
RenderFrame makeFrame(bool hybrid) {
	RenderFrame frame;
	frame.hybrid = hybrid;
	frame.width = 1280;
	frame.height = 720;
	frame.view = Mat4(1.0f);
	frame.proj = Mat4(1.0f);
	return frame;
}

/// @brief The pass names in the order they are registered.
const std::vector<const char*>& allPasses() {
	static const std::vector<const char*> names = {
		RenderPass::Shadow, RenderPass::RtScene, RenderPass::Scene, RenderPass::GBuffer,
		RenderPass::Resolve, RenderPass::Trace, RenderPass::Denoise, RenderPass::Compose,
		RenderPass::UiPost, RenderPass::PostExecute, RenderPass::UiLate, RenderPass::DebugUi,
		RenderPass::Present,
	};
	return names;
}

} // namespace

// ----------------------------------------------------------------------
// Graph construction
// ----------------------------------------------------------------------

TEST(RenderPipelineTest, RegistersEveryPass) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	EXPECT_EQ(pipeline.taskCount(), allPasses().size());
	for (const char* name : allPasses()) {
		EXPECT_TRUE(pipeline.hasPass(name)) << "missing pass: " << name;
		EXPECT_TRUE(pipeline.passId(name).isValid()) << "no id for pass: " << name;
	}
	EXPECT_FALSE(pipeline.hasPass("notAPass"));
	EXPECT_FALSE(pipeline.passId("notAPass").isValid());
}

TEST(RenderPipelineTest, GraphBuildsWithoutCycles) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);
	// The constructor builds; an invalid graph would have reported an error.
	EXPECT_TRUE(pipeline.isBuilt());
	EXPECT_TRUE(pipeline.build().isOk());
}

TEST(RenderPipelineTest, GpuStatsCoverEveryTimedPass) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	// One GPU timing slot per pass, minus present: that pass submits and waits for
	// the frame instead of doing GPU work, so its timestamps would fall on either
	// side of the submission. Slots are handed out in registration order, so this
	// also guards the wrap order against a pass being (un)wrapped by accident.
	ASSERT_EQ(pipeline.passStats().size(), allPasses().size());
	EXPECT_EQ(pipeline.passGpuStats().size() + 1, pipeline.passStats().size());

	EXPECT_TRUE(pipeline.gpuTiming());
	pipeline.setGpuTiming(false);
	EXPECT_FALSE(pipeline.gpuTiming());
	for (const auto& gpu : pipeline.passGpuStats()) {
		EXPECT_DOUBLE_EQ(gpu.lastMs, 0.0);
		EXPECT_DOUBLE_EQ(gpu.averageMs, 0.0);
		EXPECT_DOUBLE_EQ(gpu.maxMs, 0.0);
	}
	pipeline.setGpuTiming(true);
	EXPECT_TRUE(pipeline.gpuTiming());
}

TEST(RenderPipelineTest, DependenciesProduceTheExpectedLevels) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);
	ASSERT_TRUE(pipeline.isBuilt());

	const auto& stats = pipeline.passStats();
	auto levelOf = [&](const char* name) -> UInt32 {
		return stats[pipeline.passId(name).index].level;
	};

	// Independent roots.
	EXPECT_EQ(levelOf(RenderPass::Shadow), 0u);
	EXPECT_EQ(levelOf(RenderPass::RtScene), 0u);
	EXPECT_EQ(levelOf(RenderPass::GBuffer), 0u);
	// scene after shadow; resolve after gbuffer.
	EXPECT_EQ(levelOf(RenderPass::Scene), 1u);
	EXPECT_EQ(levelOf(RenderPass::Resolve), 1u);
	// trace after resolve and rtScene; the rest is a chain.
	EXPECT_EQ(levelOf(RenderPass::Trace), 2u);
	EXPECT_EQ(levelOf(RenderPass::Denoise), 3u);
	EXPECT_EQ(levelOf(RenderPass::Compose), 4u);
	EXPECT_EQ(levelOf(RenderPass::UiPost), 5u);
	EXPECT_EQ(levelOf(RenderPass::PostExecute), 6u);
	EXPECT_EQ(levelOf(RenderPass::UiLate), 7u);
	EXPECT_EQ(levelOf(RenderPass::DebugUi), 8u);
	EXPECT_EQ(levelOf(RenderPass::Present), 9u);

	EXPECT_EQ(pipeline.lastRunStats().levelCount, 0u) << "nothing has run yet";
}

TEST(RenderPipelineTest, DescribeListsEveryPass) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);
	const String text = pipeline.describe();

	for (const char* name : allPasses()) {
		EXPECT_NE(text.find(name), String::npos) << "describe() omitted " << name;
	}
	EXPECT_NE(text.find("after=shadow"), String::npos);
	EXPECT_NE(text.find("after=resolve,rtScene"), String::npos);
}

// ----------------------------------------------------------------------
// Gating: no pass may run while its subsystem is down
// ----------------------------------------------------------------------

TEST(RenderPipelineTest, EveryPassIsSkippedWhileSubsystemsAreNotRunning) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	RenderFrame frame = makeFrame(false);
	ASSERT_TRUE(pipeline.render(frame).isOk());

	const auto& stats = pipeline.passStats();
	for (const auto& pass : stats) {
		EXPECT_EQ(pass.status, PipelineTaskStatus::Skipped)
			<< "pass '" << pass.name << "' ran against an uninitialized subsystem";
	}
	EXPECT_EQ(pipeline.lastRunStats().tasksRun, 0u);
	EXPECT_EQ(pipeline.lastRunStats().tasksSkipped, static_cast<UInt32>(allPasses().size()));
}

TEST(RenderPipelineTest, HybridFrameStillSkipsEverything) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	RenderFrame frame = makeFrame(true);
	ASSERT_TRUE(pipeline.render(frame).isOk());
	EXPECT_EQ(pipeline.lastRunStats().tasksRun, 0u);
}

// ----------------------------------------------------------------------
// Frame mode selects the passes
// ----------------------------------------------------------------------

TEST(RenderPipelineTest, RasterFrameEnablesOnlyTheRasterPasses) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	RenderFrame frame = makeFrame(false);
	ASSERT_TRUE(pipeline.render(frame).isOk());

	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Shadow)));
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Scene)));
	// The hybrid passes are off, so the four-cascade shadow pass is real work
	// that a hybrid frame does not pay for.
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::RtScene)));
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::GBuffer)));
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Trace)));
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Compose)));
	// The shared tail always runs.
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::UiPost)));
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Present)));
}

TEST(RenderPipelineTest, HybridFrameSwapsTheGeometryPasses) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	RenderFrame frame = makeFrame(true);
	ASSERT_TRUE(pipeline.render(frame).isOk());

	// Shadow maps are dead work in hybrid mode: the G-buffer shaders never
	// sample them and the shadows come from the ray trace.
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Shadow)));
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Scene)));
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::RtScene)));
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::GBuffer)));
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Trace)));
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Compose)));
}

TEST(RenderPipelineTest, ResolvePassFollowsTheMsaaSampleCount) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	// An uninitialized renderer reports 1 sample, so the resolve is not needed.
	RenderFrame frame = makeFrame(true);
	ASSERT_TRUE(pipeline.render(frame).isOk());
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Resolve)));

	// Without denoising the denoise pass is off as well.
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Denoise)));
	frame.denoise = false;
	ASSERT_TRUE(pipeline.render(frame).isOk());
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Denoise)));
}

// ----------------------------------------------------------------------
// Gating a pass by hand
// ----------------------------------------------------------------------

TEST(RenderPipelineTest, TailPassesCanBeDisabledByName) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	ASSERT_TRUE(pipeline.setPassEnabled(RenderPass::Present, false).isOk());
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Present)));
	// Disabling a pass does not disable anything else: a skipped dependency is
	// not a failure, so its dependents still run (there are none here, and the
	// pass before it must be unaffected).
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::DebugUi)));
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::UiLate)));

	RenderFrame frame = makeFrame(false);
	ASSERT_TRUE(pipeline.render(frame).isOk());
	EXPECT_EQ(pipeline.passStats()[pipeline.passId(RenderPass::Present).index].status,
		PipelineTaskStatus::Skipped);

	// The per-frame mode setup only touches the mode-driven passes, so a manual
	// setting survives render().
	ASSERT_TRUE(pipeline.setPassEnabled(RenderPass::Present, true).isOk());
	EXPECT_TRUE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::Present)));

	// The event passes the caller can turn off are covered too.
	ASSERT_TRUE(pipeline.setPassEnabled(RenderPass::PostExecute, false).isOk());
	EXPECT_FALSE(pipeline.isTaskEnabled(pipeline.passId(RenderPass::PostExecute)));
	ASSERT_TRUE(pipeline.setPassEnabled(RenderPass::PostExecute, true).isOk());
}

TEST(RenderPipelineTest, EveryPassIsGatedByASubsystem) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	// Each pass must name the subsystem it drives, otherwise it would run
	// against a subsystem that is not up. The scene passes answer to the
	// renderer; the rest to the subsystem they belong to.
	for (const auto& pass : pipeline.passStats()) {
		EXPECT_NE(pass.subsystem, nullptr) << "pass '" << pass.name << "' has no subsystem";
	}
	EXPECT_EQ(pipeline.passStats()[pipeline.passId(RenderPass::Shadow).index].subsystem,
		static_cast<Subsystem*>(&fx.shadow));
	EXPECT_EQ(pipeline.passStats()[pipeline.passId(RenderPass::Scene).index].subsystem,
		static_cast<Subsystem*>(&fx.renderer));
	EXPECT_EQ(pipeline.passStats()[pipeline.passId(RenderPass::GBuffer).index].subsystem,
		static_cast<Subsystem*>(&fx.renderer));
	EXPECT_EQ(pipeline.passStats()[pipeline.passId(RenderPass::Trace).index].subsystem,
		static_cast<Subsystem*>(&fx.rayTracing));
	// The denoise pass answers to the RT subsystem: it falls back to the ray
	// tracer's own filter when NRD is unavailable, so it must not be gated on NRD.
	EXPECT_EQ(pipeline.passStats()[pipeline.passId(RenderPass::Denoise).index].subsystem,
		static_cast<Subsystem*>(&fx.rayTracing));
	EXPECT_EQ(pipeline.passStats()[pipeline.passId(RenderPass::Present).index].subsystem,
		static_cast<Subsystem*>(&fx.postProcess));
}

TEST(RenderPipelineTest, UnknownPassNameIsRejected) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	const auto result = pipeline.setPassEnabled("noSuchPass", true);
	ASSERT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), CoreError::InvalidArgument);
}

// ----------------------------------------------------------------------
// Frame bookkeeping
// ----------------------------------------------------------------------

TEST(RenderPipelineTest, RenderFillsDerivedFrameFields) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	RenderFrame frame = makeFrame(true);
	frame.gBufferColor = TextureHandle{ 1, 1 };
	frame.gBufferNormal = TextureHandle{ 2, 1 };
	frame.gBufferEmissive = TextureHandle{ 3, 1 };
	frame.gBufferDepth = TextureHandle{ 4, 1 };
	frame.rtWidth = 0;
	frame.rtHeight = 0;

	ASSERT_TRUE(pipeline.render(frame).isOk());

	// With no resolve pass the guide textures stay the G-buffer itself.
	EXPECT_EQ(frame.sourceColor.index, frame.gBufferColor.index);
	EXPECT_EQ(frame.sourceNormal.index, frame.gBufferNormal.index);
	EXPECT_EQ(frame.sourceEmissive.index, frame.gBufferEmissive.index);
	EXPECT_EQ(frame.sourceDepth.index, frame.gBufferDepth.index);
	// The RT resolution defaults to the render resolution.
	EXPECT_EQ(frame.rtWidth, frame.width);
	EXPECT_EQ(frame.rtHeight, frame.height);
	// Trace constants are kept consistent with the camera.
	const Mat4 expectedInv = glm::inverse(frame.proj * frame.view);
	EXPECT_EQ(frame.rtConstants.viewProjInv, expectedInv);
}

TEST(RenderPipelineTest, RenderIsANoOpWhenDisabled) {
	RenderFixture fx;
	RenderPipeline pipeline(fx.executor, fx.context);

	pipeline.setEnabled(false);
	RenderFrame frame = makeFrame(false);
	ASSERT_TRUE(pipeline.render(frame).isOk());
	EXPECT_EQ(pipeline.lastRunStats().tasksRun, 0u);
	EXPECT_EQ(pipeline.lastRunStats().tasksSkipped, 0u) << "a disabled pipeline does not even evaluate passes";
}
