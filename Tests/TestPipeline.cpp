#include <Engine/Core/Pipeline.hpp>
#include <Engine/Core/PipelineBase.hpp>
#include <Engine/Jobs/JobExecutor.hpp>
#include <Engine/Jobs/JobSubsystem.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace EnderEngine;
using namespace EnderEngine::Jobs;

namespace {

/// @brief A JobSubsystem with a couple of workers plus the matching executor.
struct TestJobs {
	JobSubsystem jobs;
	JobExecutor executor{ jobs };

	TestJobs() {
		jobs.setWorkerThreadCount(3);
		jobs.initialize();
	}
	~TestJobs() { jobs.shutdown(); }
};

/// @brief Records the order in which tasks executed.
struct Trace {
	std::mutex mutex;
	std::vector<String> order;

	void add(StringView name) {
		std::lock_guard<std::mutex> lock(mutex);
		order.push_back(String(name));
	}

	/// @return Index of the entry, or -1.
	int indexOf(StringView name) const {
		for (size_t i = 0; i < order.size(); ++i) {
			if (order[i] == name) return static_cast<int>(i);
		}
		return -1;
	}
};

} // namespace

// ----------------------------------------------------------------------
// Construction and validation
// ----------------------------------------------------------------------

TEST(PipelineTest, RejectsInvalidDescriptors) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Invalid");

	// No function.
	EXPECT_FALSE(pipeline.addTask("noFn", nullptr).isValid());
	// Empty name.
	EXPECT_FALSE(pipeline.addTask("", [](const PipelineFrameContext&) { return Result<void, CoreError>{}; }).isValid());
	// Duplicate name.
	EXPECT_TRUE(pipeline.addTask("dup", [](const PipelineFrameContext&) { return Result<void, CoreError>{}; }).isValid());
	EXPECT_FALSE(pipeline.addTask("dup", [](const PipelineFrameContext&) { return Result<void, CoreError>{}; }).isValid());

	EXPECT_EQ(pipeline.taskCount(), 1u);
}

TEST(PipelineTest, BuildRejectsEmptyAndUnknownDependencies) {
	TestJobs tj;
	{
		Pipeline empty(tj.executor, "Empty");
		EXPECT_TRUE(empty.build().isErr());
	}
	{
		// dependsOn() rejects a bad id outright, which leaves the graph valid.
		Pipeline pipeline(tj.executor, "UnknownDep");
		PipelineTaskId a = pipeline.addTask("a", [](const PipelineFrameContext&) { return Result<void, CoreError>{}; });
		ASSERT_TRUE(a.isValid());
		EXPECT_TRUE(pipeline.dependsOn(a, PipelineTaskId{ 99 }).isErr());
		EXPECT_TRUE(pipeline.build().isOk());
	}
	{
		// A bad id injected through the descriptor must be caught by build().
		Pipeline pipeline(tj.executor, "BadDescriptorDep");
		PipelineTaskDesc desc;
		desc.name = "a";
		desc.after = { PipelineTaskId{ 99 } };
		desc.fn = [](const PipelineFrameContext&) { return Result<void, CoreError>{}; };
		ASSERT_TRUE(pipeline.addTask(desc).isValid());
		EXPECT_TRUE(pipeline.build().isErr());
		EXPECT_FALSE(pipeline.isBuilt());
	}
}

TEST(PipelineTest, BuildRejectsCycles) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Cyclic");
	auto fn = [](const PipelineFrameContext&) { return Result<void, CoreError>{}; };
	PipelineTaskId a = pipeline.addTask("a", fn);
	PipelineTaskId b = pipeline.addTask("b", fn);
	PipelineTaskId c = pipeline.addTask("c", fn);
	ASSERT_TRUE(pipeline.dependsOn(b, a).isOk());
	ASSERT_TRUE(pipeline.dependsOn(c, b).isOk());
	ASSERT_TRUE(pipeline.dependsOn(a, c).isOk());
	EXPECT_TRUE(pipeline.build().isErr());
	EXPECT_FALSE(pipeline.isBuilt());
	// A pipeline that failed to build must refuse to run.
	EXPECT_EQ(pipeline.run(0.016).error(), CoreError::NotInitialized);
}

TEST(PipelineTest, BuildComputesLevels) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Levels");
	auto fn = [](const PipelineFrameContext&) { return Result<void, CoreError>{}; };
	PipelineTaskId a = pipeline.addTask("a", fn);
	PipelineTaskId b = pipeline.addTask("b", fn);
	PipelineTaskId c = pipeline.addTask("c", fn);
	PipelineTaskId d = pipeline.addTask("d", fn);
	ASSERT_TRUE(pipeline.dependsOn(b, a).isOk());
	ASSERT_TRUE(pipeline.dependsOn(c, a).isOk());
	ASSERT_TRUE(pipeline.dependsOn(d, { b, c }).isOk());
	ASSERT_TRUE(pipeline.build().isOk());

	const auto& stats = pipeline.taskStats();
	ASSERT_EQ(stats.size(), 4u);
	EXPECT_EQ(stats[a.index].level, 0u);
	EXPECT_EQ(stats[b.index].level, 1u);
	EXPECT_EQ(stats[c.index].level, 1u);
	EXPECT_EQ(stats[d.index].level, 2u);
	EXPECT_EQ(pipeline.lastRunStats().levelCount, 0u); // nothing has run yet

	const String text = pipeline.describe();
	EXPECT_NE(text.find("level 0"), String::npos);
	EXPECT_NE(text.find("level 2"), String::npos);
	EXPECT_NE(text.find("after=b,c"), String::npos);
}

TEST(PipelineTest, AddingTaskAfterBuildInvalidatesIt) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Rebuild");
	auto fn = [](const PipelineFrameContext&) { return Result<void, CoreError>{}; };
	pipeline.addTask("a", fn);
	ASSERT_TRUE(pipeline.build().isOk());
	EXPECT_TRUE(pipeline.isBuilt());

	pipeline.addTask("b", fn);
	EXPECT_FALSE(pipeline.isBuilt());
	EXPECT_EQ(pipeline.run(0.016).error(), CoreError::NotInitialized);
	EXPECT_TRUE(pipeline.build().isOk());
	EXPECT_TRUE(pipeline.run(0.016).isOk());
}

// ----------------------------------------------------------------------
// Ordering
// ----------------------------------------------------------------------

TEST(PipelineTest, MainThreadTasksRespectDependencies) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Order");
	Trace trace;
	auto fn = [&trace](StringView name) {
		return [&trace, name](const PipelineFrameContext&) -> Result<void, CoreError> {
			trace.add(name);
			return {};
		};
	};

	// Registered in an order that does not match the dependency order.
	PipelineTaskId draw = pipeline.addTask("draw", fn("draw"));
	PipelineTaskId sim = pipeline.addTask("sim", fn("sim"));
	PipelineTaskId present = pipeline.addTask("present", fn("present"));
	ASSERT_TRUE(pipeline.dependsOn(draw, sim).isOk());
	ASSERT_TRUE(pipeline.dependsOn(present, draw).isOk());
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	ASSERT_EQ(trace.order.size(), 3u);
	EXPECT_LT(trace.indexOf("sim"), trace.indexOf("draw"));
	EXPECT_LT(trace.indexOf("draw"), trace.indexOf("present"));
}

TEST(PipelineTest, MainThreadTasksRunOnTheCallingThread) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Thread");
	const std::thread::id caller = std::this_thread::get_id();
	std::thread::id observed{};
	pipeline.addTask("main", [&observed](const PipelineFrameContext&) -> Result<void, CoreError> {
		observed = std::this_thread::get_id();
		return {};
	});
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(observed, caller);
}

TEST(PipelineTest, MainThreadOnlyPipelineIsDeterministic) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Deterministic");
	std::vector<String> order;
	for (int i = 0; i < 8; ++i) {
		pipeline.addTask("t" + std::to_string(i), [&order, i](const PipelineFrameContext&) -> Result<void, CoreError> {
			order.push_back("t" + std::to_string(i));
			return {};
		});
	}
	ASSERT_TRUE(pipeline.build().isOk());
	for (int frame = 0; frame < 3; ++frame) {
		order.clear();
		ASSERT_TRUE(pipeline.run(0.016).isOk());
		ASSERT_EQ(order.size(), 8u);
		for (int i = 0; i < 8; ++i) EXPECT_EQ(order[i], "t" + std::to_string(i));
	}
}

// ----------------------------------------------------------------------
// Explicit parallelism
// ----------------------------------------------------------------------

TEST(PipelineTest, ParallelTasksActuallyOverlap) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Overlap");

	std::atomic<int> concurrent{ 0 };
	std::atomic<int> maxConcurrent{ 0 };
	auto body = [&concurrent, &maxConcurrent](const PipelineFrameContext&) -> Result<void, CoreError> {
		const int now = concurrent.fetch_add(1, std::memory_order_acq_rel) + 1;
		int previous = maxConcurrent.load(std::memory_order_relaxed);
		while (now > previous && !maxConcurrent.compare_exchange_weak(previous, now, std::memory_order_relaxed)) {}
		// Wait (bounded by a deadline) until the peer shows up. If the scheduler
		// serialised the two tasks the deadline expires and the assertion below
		// fails, instead of the test hanging.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
		while (concurrent.load(std::memory_order_acquire) < 2 && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::yield();
		}
		concurrent.fetch_sub(1, std::memory_order_acq_rel);
		return {};
	};

	PipelineTaskDesc first;
	first.name = "first";
	first.mode = PipelineTaskMode::Parallel;
	first.fn = body;
	PipelineTaskDesc second;
	second.name = "second";
	second.mode = PipelineTaskMode::Parallel;
	second.fn = body;
	pipeline.addTask(first);
	pipeline.addTask(second);
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	EXPECT_GE(maxConcurrent.load(), 2) << "independent Parallel tasks did not overlap";
}

TEST(PipelineTest, TaskStartsOnlyAfterItsDependenciesFinish) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Dependency");
	std::atomic<bool> producerDone{ false };
	std::atomic<bool> sawProducerDone{ false };

	PipelineTaskDesc producer;
	producer.name = "producer";
	producer.mode = PipelineTaskMode::Parallel;
	producer.fn = [&producerDone](const PipelineFrameContext&) -> Result<void, CoreError> {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		producerDone.store(true, std::memory_order_release);
		return {};
	};

	PipelineTaskDesc consumer;
	consumer.name = "consumer";
	consumer.mode = PipelineTaskMode::Parallel;
	consumer.fn = [&producerDone, &sawProducerDone](const PipelineFrameContext&) -> Result<void, CoreError> {
		sawProducerDone.store(producerDone.load(std::memory_order_acquire), std::memory_order_release);
		return {};
	};

	PipelineTaskId p = pipeline.addTask(producer);
	PipelineTaskId c = pipeline.addTask(consumer);
	ASSERT_TRUE(pipeline.dependsOn(c, p).isOk());
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	EXPECT_TRUE(sawProducerDone.load()) << "a dependent task ran before its dependency finished";
}

TEST(PipelineTest, ParallelForPreservesJobIndices) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "ParallelFor");
	constexpr UInt32 kCount = 32;
	std::atomic<int> seen[kCount];
	for (auto& s : seen) s.store(0);

	PipelineTaskDesc desc;
	desc.name = "for";
	desc.mode = PipelineTaskMode::Parallel;
	desc.jobCount = kCount;
	desc.groupSize = 4;
	desc.fn = [&seen](const PipelineFrameContext& context) -> Result<void, CoreError> {
		if (context.jobCount != kCount) return CoreError::OperationFailed;
		if (context.jobIndex >= kCount) return CoreError::OperationFailed;
		seen[context.jobIndex].fetch_add(1);
		return {};
	};
	pipeline.addTask(desc);
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	for (UInt32 i = 0; i < kCount; ++i) {
		EXPECT_EQ(seen[i].load(), 1) << "job index " << i << " was not visited exactly once";
	}
}

// ----------------------------------------------------------------------
// Gating
// ----------------------------------------------------------------------

TEST(PipelineTest, DisabledTaskIsSkippedButDependentsStillRun) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Enabled");
	std::atomic<int> ran{ 0 };
	auto fn = [&ran](const PipelineFrameContext&) -> Result<void, CoreError> { ran.fetch_add(1); return {}; };

	PipelineTaskId a = pipeline.addTask("a", fn);
	PipelineTaskId b = pipeline.addTask("b", fn);
	ASSERT_TRUE(pipeline.dependsOn(b, a).isOk());
	ASSERT_TRUE(pipeline.build().isOk());

	pipeline.setTaskEnabled(a, false);
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 1); // only "b"
	EXPECT_EQ(pipeline.taskStats()[a.index].status, PipelineTaskStatus::Skipped);
	EXPECT_EQ(pipeline.taskStats()[b.index].status, PipelineTaskStatus::Succeeded);
	EXPECT_EQ(pipeline.lastRunStats().tasksSkipped, 1u);

	pipeline.setTaskEnabled(a, true);
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 3);
}

TEST(PipelineTest, ConditionGateSkipsTheTask) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Gate");
	std::atomic<int> ran{ 0 };
	std::atomic<bool> allow{ false };

	PipelineTaskDesc desc;
	desc.name = "gated";
	desc.fn = [&ran](const PipelineFrameContext&) -> Result<void, CoreError> { ran.fetch_add(1); return {}; };
	desc.when = [&allow](const PipelineFrameContext&) { return allow.load(); };
	pipeline.addTask(desc);
	ASSERT_TRUE(pipeline.build().isOk());

	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 0);
	EXPECT_EQ(pipeline.taskStats()[0].status, PipelineTaskStatus::Skipped);

	allow.store(true);
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 1);
	EXPECT_EQ(pipeline.taskStats()[0].status, PipelineTaskStatus::Succeeded);
}

TEST(PipelineTest, DisabledPipelineIsANoOp) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Off");
	std::atomic<int> ran{ 0 };
	pipeline.addTask("a", [&ran](const PipelineFrameContext&) -> Result<void, CoreError> { ran.fetch_add(1); return {}; });
	ASSERT_TRUE(pipeline.build().isOk());

	pipeline.setEnabled(false);
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 0);
}

TEST(PipelineTest, TaskOfANonRunningSubsystemIsSkipped) {
	TestJobs tj;
	Subsystem probe("Probe"); // never initialized -> state is Initialized
	std::atomic<int> ran{ 0 };

	Pipeline pipeline(tj.executor, "Subsystem");
	PipelineTaskDesc desc;
	desc.name = "probe";
	desc.subsystem = &probe;
	desc.fn = [&ran](const PipelineFrameContext&) -> Result<void, CoreError> { ran.fetch_add(1); return {}; };
	pipeline.addTask(desc);
	ASSERT_TRUE(pipeline.build().isOk());

	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 0) << "a task must not be called while its subsystem is not running";

	ASSERT_TRUE(probe.initialize().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 1);

	probe.shutdown();
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 1) << "a task must not be called after its subsystem shut down";
}

// ----------------------------------------------------------------------
// Errors
// ----------------------------------------------------------------------

TEST(PipelineTest, FailureSkipsDependentsAndKeepsIndependents) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Failure");
	std::atomic<int> independent{ 0 };

	PipelineTaskId failing = pipeline.addTask("failing", [](const PipelineFrameContext&) -> Result<void, CoreError> {
		return CoreError::OperationFailed;
	});
	// Would succeed on its own, but must never be reached.
	PipelineTaskId dependent = pipeline.addTask("dependent", [](const PipelineFrameContext&) -> Result<void, CoreError> {
		return {};
	});
	std::atomic<int> dependentRan{ 0 };
	PipelineTaskDesc dependentDesc;
	dependentDesc.name = "dependent2";
	dependentDesc.fn = [&dependentRan](const PipelineFrameContext&) -> Result<void, CoreError> {
		dependentRan.fetch_add(1);
		return {};
	};
	PipelineTaskId dependent2 = pipeline.addTask(dependentDesc);

	PipelineTaskDesc independentDesc;
	independentDesc.name = "independent";
	independentDesc.fn = [&independent](const PipelineFrameContext&) -> Result<void, CoreError> {
		independent.fetch_add(1);
		return {};
	};
	pipeline.addTask(independentDesc);

	ASSERT_TRUE(pipeline.dependsOn(dependent, failing).isOk());
	ASSERT_TRUE(pipeline.dependsOn(dependent2, failing).isOk());
	ASSERT_TRUE(pipeline.build().isOk());

	const auto result = pipeline.run(0.016);
	ASSERT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), CoreError::OperationFailed);

	EXPECT_EQ(pipeline.taskStats()[failing.index].status, PipelineTaskStatus::Failed);
	EXPECT_EQ(pipeline.taskStats()[dependent.index].status, PipelineTaskStatus::Skipped);
	EXPECT_EQ(pipeline.taskStats()[dependent2.index].status, PipelineTaskStatus::Skipped);
	EXPECT_EQ(dependentRan.load(), 0);
	EXPECT_EQ(independent.load(), 1) << "an independent task must still run";
	EXPECT_EQ(pipeline.lastRunStats().tasksFailed, 1u);
}

TEST(PipelineTest, StopOnErrorHaltsTheRemainder) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "StopOnError");
	std::atomic<int> laterRan{ 0 };

	PipelineTaskId failing = pipeline.addTask("failing", [](const PipelineFrameContext&) -> Result<void, CoreError> {
		return CoreError::OperationFailed;
	});
	PipelineTaskId later = pipeline.addTask("later", [&laterRan](const PipelineFrameContext&) -> Result<void, CoreError> {
		laterRan.fetch_add(1);
		return {};
	});
	ASSERT_TRUE(pipeline.dependsOn(later, failing).isOk());
	ASSERT_TRUE(pipeline.build().isOk());

	pipeline.setStopOnError(true);
	EXPECT_TRUE(pipeline.stopOnError());
	EXPECT_TRUE(pipeline.run(0.016).isErr());
	EXPECT_EQ(laterRan.load(), 0);
}

// ----------------------------------------------------------------------
// Context, statistics and nesting
// ----------------------------------------------------------------------

TEST(PipelineTest, RunManagesFrameTiming) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Timing");
	UInt64 seenFrame = 999;
	F64 seenDelta = 0.0;
	F64 seenElapsed = 0.0;
	pipeline.addTask("a", [&](const PipelineFrameContext& context) -> Result<void, CoreError> {
		seenFrame = context.frameIndex;
		seenDelta = context.deltaTime;
		seenElapsed = context.elapsedTime;
		return {};
	});
	ASSERT_TRUE(pipeline.build().isOk());

	ASSERT_TRUE(pipeline.run(0.25).isOk());
	EXPECT_EQ(seenFrame, 0u);
	EXPECT_DOUBLE_EQ(seenDelta, 0.25);
	EXPECT_DOUBLE_EQ(seenElapsed, 0.25);
	EXPECT_EQ(pipeline.lastRunStats().frameIndex, 0u);

	ASSERT_TRUE(pipeline.run(0.5).isOk());
	EXPECT_EQ(seenFrame, 1u);
	EXPECT_DOUBLE_EQ(seenElapsed, 0.75);
}

TEST(PipelineTest, ExplicitContextIsPassedThrough) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Context");
	int payload = 0;
	int observed = 0;
	PipelineTaskDesc desc;
	desc.name = "a";
	desc.fn = [&observed](const PipelineFrameContext& context) -> Result<void, CoreError> {
		observed = *static_cast<int*>(context.payload);
		return {};
	};
	pipeline.addTask(desc);
	ASSERT_TRUE(pipeline.build().isOk());

	payload = 42;
	PipelineFrameContext context;
	context.payload = &payload;
	context.frameIndex = 7;
	ASSERT_TRUE(pipeline.run(context).isOk());
	EXPECT_EQ(observed, 42);
	EXPECT_EQ(pipeline.lastRunStats().frameIndex, 7u);
}

TEST(PipelineTest, StatisticsAreCollected) {
	TestJobs tj;
	Pipeline pipeline(tj.executor, "Stats");
	pipeline.addTask("slow", [](const PipelineFrameContext&) -> Result<void, CoreError> {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		return {};
	});
	PipelineTaskDesc par;
	par.name = "par";
	par.mode = PipelineTaskMode::Parallel;
	par.fn = [](const PipelineFrameContext&) -> Result<void, CoreError> { return {}; };
	pipeline.addTask(par);
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	const auto& run = pipeline.lastRunStats();
	EXPECT_EQ(run.tasksRun, 2u);
	EXPECT_EQ(run.tasksSkipped, 0u);
	EXPECT_EQ(run.tasksFailed, 0u);
	EXPECT_GT(run.totalMs, 0.0);
	EXPECT_GE(run.mainThreadMs, 4.0);
	EXPECT_GE(run.maxLevelWidth, 2u);

	const auto& stats = pipeline.taskStats();
	EXPECT_GE(stats[0].lastMs, 4.0);
	EXPECT_GE(stats[0].averageMs, 4.0);
	EXPECT_EQ(stats[0].runCount, 1u);

	pipeline.resetStats();
	EXPECT_EQ(pipeline.lastRunStats().totalMs, 0.0);
	EXPECT_EQ(pipeline.taskStats()[0].runCount, 0u);
}

TEST(PipelineTest, NestedPipelineRunsAsOneTask) {
	TestJobs tj;
	Pipeline child(tj.executor, "Child");
	std::atomic<int> childRan{ 0 };
	child.addTask("childTask", [&childRan](const PipelineFrameContext&) -> Result<void, CoreError> {
		childRan.fetch_add(1);
		return {};
	});
	ASSERT_TRUE(child.build().isOk());

	Pipeline parent(tj.executor, "Parent");
	std::atomic<int> parentRan{ 0 };
	PipelineTaskId childTask = parent.addPipeline(child);
	parent.addTask("parentTask", [&parentRan](const PipelineFrameContext&) -> Result<void, CoreError> {
		parentRan.fetch_add(1);
		return {};
	});
	ASSERT_TRUE(parent.build().isOk());
	ASSERT_TRUE(parent.run(0.016).isOk());

	EXPECT_EQ(childRan.load(), 1);
	EXPECT_EQ(parentRan.load(), 1);
	EXPECT_EQ(parent.taskStats()[childTask.index].status, PipelineTaskStatus::Succeeded);
}

// ----------------------------------------------------------------------
// The executor is a real seam, not just a type alias
// ----------------------------------------------------------------------

namespace {

/// @brief Minimal third-party executor: runs batches inline and counts them.
///
/// Its only purpose is to show that PipelineBase works with an execution engine
/// it has never heard of, as long as the engine satisfies PipelineExecutor.
class CountingExecutor {
public:
	struct Handle {
		UInt32 id = 0;
		EE_NODISCARD bool isValid() const { return id != 0; }
	};
	using Priority = int;

	struct TaskDesc {
		std::function<Result<void, CoreError>(const PipelineInvocation&)> fn;
		UInt32 jobCount = 1;
		UInt32 groupSize = 1;
		Priority priority = 0;
		Vector<Handle> dependencies;
	};

	Result<Handle, CoreError> dispatch(const TaskDesc& desc) {
		++dispatchCount;
		lastDependencyCount = static_cast<UInt32>(desc.dependencies.size());
		if (!desc.fn) return CoreError::InvalidArgument;
		const UInt32 jobCount = (desc.jobCount > 0) ? desc.jobCount : 1u;
		for (UInt32 i = 0; i < jobCount; ++i) {
			PipelineInvocation invocation;
			invocation.jobIndex = i;
			invocation.jobCount = jobCount;
			(void)desc.fn(invocation);
		}
		return Handle{ ++m_next };
	}

	void wait(Handle) {}

	UInt32 dispatchCount = 0;
	UInt32 lastDependencyCount = 0;

private:
	UInt32 m_next = 0;
};

static_assert(PipelineExecutor<CountingExecutor>, "CountingExecutor must satisfy the executor contract");
static_assert(PipelineExecutor<SequentialExecutor>, "SequentialExecutor must satisfy the executor contract");
static_assert(PipelineExecutor<JobExecutor>, "JobExecutor must satisfy the executor contract");

} // namespace

TEST(PipelineExecutorTest, CustomExecutorDrivesPipelineBase) {
	CountingExecutor executor;
	using CountingPipeline = PipelineBase<CountingExecutor>;
	CountingPipeline pipeline(executor, "Custom");

	std::atomic<int> ran{ 0 };
	pipeline.addTask("a", [&ran](const PipelineFrameContext&) -> Result<void, CoreError> {
		ran.fetch_add(1);
		return {};
	});
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());
	EXPECT_EQ(ran.load(), 1);
	EXPECT_EQ(executor.dispatchCount, 0u) << "a main-thread task must not go through the executor";
}

TEST(PipelineExecutorTest, CustomExecutorReceivesParallelDependencies) {
	CountingExecutor executor;
	using CountingPipeline = PipelineBase<CountingExecutor>;
	CountingPipeline pipeline(executor, "CustomDeps");
	auto fn = [](const PipelineFrameContext&) -> Result<void, CoreError> { return {}; };

	// The descriptor is a member of the pipeline: its priority field is typed by
	// the executor, so a custom engine brings its own descriptor type.
	CountingPipeline::TaskDesc producer;
	producer.name = "producer";
	producer.mode = PipelineTaskMode::Parallel;
	producer.fn = fn;
	CountingPipeline::TaskDesc consumer;
	consumer.name = "consumer";
	consumer.mode = PipelineTaskMode::Parallel;
	consumer.fn = fn;

	PipelineTaskId p = pipeline.addTask(producer);
	PipelineTaskId c = pipeline.addTask(consumer);
	ASSERT_TRUE(pipeline.dependsOn(c, p).isOk());
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	EXPECT_EQ(executor.dispatchCount, 2u);
	// The dependent batch must have been told about the producer's handle.
	EXPECT_EQ(executor.lastDependencyCount, 1u);
}

// ----------------------------------------------------------------------
// SequentialExecutor: PipelineBase with no Jobs dependency at all
// ----------------------------------------------------------------------

TEST(SequentialExecutorTest, RunsEverythingInlineOnTheCallingThread) {
	SequentialExecutor executor;
	SequentialPipeline pipeline(executor, "Inline");

	const std::thread::id caller = std::this_thread::get_id();
	std::thread::id mainObserved{};
	std::thread::id parallelObserved{};
	std::atomic<int> ran{ 0 };

	SequentialPipeline::TaskDesc parallel;
	parallel.name = "parallel";
	parallel.mode = PipelineTaskMode::Parallel;
	parallel.fn = [&parallelObserved, &ran](const PipelineFrameContext&) -> Result<void, CoreError> {
		parallelObserved = std::this_thread::get_id();
		ran.fetch_add(1);
		return {};
	};
	pipeline.addTask(parallel);
	pipeline.addTask("main", [&mainObserved, &ran](const PipelineFrameContext&) -> Result<void, CoreError> {
		mainObserved = std::this_thread::get_id();
		ran.fetch_add(1);
		return {};
	});
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	EXPECT_EQ(ran.load(), 2);
	EXPECT_EQ(mainObserved, caller);
	EXPECT_EQ(parallelObserved, caller) << "SequentialExecutor must not spawn threads";
	EXPECT_EQ(pipeline.lastRunStats().tasksRun, 2u);
}

TEST(SequentialExecutorTest, PreservesOrderingAndDependencies) {
	SequentialExecutor executor;
	SequentialPipeline pipeline(executor, "InlineOrder");
	Trace trace;
	auto fn = [&trace](StringView name) {
		return [&trace, name](const PipelineFrameContext&) -> Result<void, CoreError> {
			trace.add(name);
			return {};
		};
	};

	PipelineTaskId sim = pipeline.addTask("sim", fn("sim"));
	PipelineTaskId draw = pipeline.addTask("draw", fn("draw"));
	PipelineTaskId present = pipeline.addTask("present", fn("present"));
	ASSERT_TRUE(pipeline.dependsOn(draw, sim).isOk());
	ASSERT_TRUE(pipeline.dependsOn(present, draw).isOk());
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	ASSERT_EQ(trace.order.size(), 3u);
	EXPECT_LT(trace.indexOf("sim"), trace.indexOf("draw"));
	EXPECT_LT(trace.indexOf("draw"), trace.indexOf("present"));
}

TEST(SequentialExecutorTest, ParallelForStillVisitsEveryJobIndex) {
	SequentialExecutor executor;
	SequentialPipeline pipeline(executor, "InlineFor");
	constexpr UInt32 kCount = 8;
	std::atomic<int> seen[kCount];
	for (auto& s : seen) s.store(0);

	SequentialPipeline::TaskDesc desc;
	desc.name = "for";
	desc.mode = PipelineTaskMode::Parallel;
	desc.jobCount = kCount;
	desc.fn = [&seen](const PipelineFrameContext& context) -> Result<void, CoreError> {
		seen[context.jobIndex].fetch_add(1);
		return {};
	};
	pipeline.addTask(desc);
	ASSERT_TRUE(pipeline.build().isOk());
	ASSERT_TRUE(pipeline.run(0.016).isOk());

	for (UInt32 i = 0; i < kCount; ++i) EXPECT_EQ(seen[i].load(), 1);
}

TEST(SequentialExecutorTest, FailurePolicyIsIdentical) {
	SequentialExecutor executor;
	SequentialPipeline pipeline(executor, "InlineFailure");
	std::atomic<int> dependentRan{ 0 };

	PipelineTaskId failing = pipeline.addTask("failing", [](const PipelineFrameContext&) -> Result<void, CoreError> {
		return CoreError::OperationFailed;
	});
	SequentialPipeline::TaskDesc dependent;
	dependent.name = "dependent";
	dependent.fn = [&dependentRan](const PipelineFrameContext&) -> Result<void, CoreError> {
		dependentRan.fetch_add(1);
		return {};
	};
	PipelineTaskId dependentId = pipeline.addTask(dependent);
	ASSERT_TRUE(pipeline.dependsOn(dependentId, failing).isOk());
	ASSERT_TRUE(pipeline.build().isOk());

	const auto result = pipeline.run(0.016);
	ASSERT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), CoreError::OperationFailed);
	EXPECT_EQ(dependentRan.load(), 0);
	EXPECT_EQ(pipeline.taskStats()[dependentId.index].status, PipelineTaskStatus::Skipped);
}
