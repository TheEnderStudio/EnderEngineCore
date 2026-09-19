#pragma once

#include "Macros.h"
#include "Types.hpp"
#include "Errors.hpp"
#include "Log.hpp"
#include "Subsystem.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <utility>

EE_NAMESPACE_BEGIN

/// @brief Index value marking an invalid PipelineTaskId.
inline constexpr UInt32 InvalidTaskIndex = 0xFFFFFFFFu;

/**
 * @brief Identifier of a task inside a PipelineBase.
 *
 * Returned by PipelineBase::addTask() and used to declare dependencies. The
 * index is stable for the lifetime of the pipeline; it is never reused.
 */
struct PipelineTaskId {
	UInt32 index = InvalidTaskIndex;

	/// @return true if the id refers to a registered task.
	EE_NODISCARD bool isValid() const { return index != InvalidTaskIndex; }

	EE_NODISCARD bool operator==(const PipelineTaskId& other) const { return index == other.index; }
	EE_NODISCARD bool operator!=(const PipelineTaskId& other) const { return index != other.index; }
};

/// @brief Id that does not refer to any task.
inline constexpr PipelineTaskId InvalidPipelineTaskId{};

/**
 * @brief How a task is executed inside a pipeline.
 *
 * PipelineTaskMode::MainThread is the safe default. Choosing
 * PipelineTaskMode::Parallel is an explicit promise by the caller that the task
 * only touches state that no concurrently running task touches - the pipeline
 * cannot verify it.
 *
 * The executor decides what Parallel means in practice: an executor with no
 * worker threads (see SequentialExecutor) runs parallel tasks inline, so
 * choosing Parallel never changes correctness, only concurrency.
 */
enum class PipelineTaskMode {
	/// @brief Runs inline, on the thread that called run().
	///
	/// Main-thread tasks never overlap each other. Their relative order is the
	/// level order of the dependency graph, and within one level the
	/// registration order, so a pipeline of only main-thread tasks is
	/// deterministic.
	MainThread,
	/// @brief Handed to the executor, which may run it concurrently.
	///
	/// May overlap with any other task whose dependencies have already reached a
	/// terminal state, including main-thread tasks. Use this only for work that
	/// shares no mutable state with those tasks.
	Parallel,
};

/// @brief Outcome of a single task during the most recent run.
enum class PipelineTaskStatus {
	NotRun,    ///< Not evaluated yet in this run.
	Skipped,   ///< Disabled, gated out, or a dependency failed.
	Succeeded, ///< Ran and returned success.
	Failed,    ///< Ran and returned an error.
};

/**
 * @brief Convert a PipelineTaskStatus to a human-readable string.
 * @param status The status value.
 * @return A null-terminated descriptive string.
 */
inline const char* ToString(PipelineTaskStatus status) {
	switch (status) {
	case PipelineTaskStatus::NotRun: return "NotRun";
	case PipelineTaskStatus::Skipped: return "Skipped";
	case PipelineTaskStatus::Succeeded: return "Succeeded";
	case PipelineTaskStatus::Failed: return "Failed";
	}
	return "Unknown";
}

/**
 * @brief Per-run data handed to every task of a pipeline.
 *
 * Passed by const reference; the pointed-to payload (and anything reachable
 * through it) may be written by tasks - that is how a producer task publishes
 * its results to its dependents.
 */
struct PipelineFrameContext {
	UInt64 frameIndex = 0;  ///< Index of this run, increasing from 0.
	F64 deltaTime = 0.0;    ///< Seconds since the previous run.
	F64 elapsedTime = 0.0;  ///< Sum of delta times, in seconds.

	/// Index of this invocation within the task, in [0, jobCount).
	/// Always 0 for tasks registered with jobCount == 1.
	UInt32 jobIndex = 0;
	/// Number of invocations of the current task (>= 1).
	UInt32 jobCount = 1;

	/**
	 * @brief Optional payload owned by the pipeline (subclass) rather than Core.
	 *
	 * Lets a derived pipeline (e.g. a render pipeline) publish a frame
	 * description without Core having to know its type. Never dereferenced by
	 * the pipeline itself.
	 */
	void* payload = nullptr;
};

/// @brief Signature of a pipeline task.
using PipelineTaskFn = std::function<Result<void, CoreError>(const PipelineFrameContext&)>;

/// @brief Optional extra gate evaluated just before a task would run.
using PipelineCondition = std::function<bool(const PipelineFrameContext&)>;

/**
 * @brief Per-invocation data an executor hands to a task body.
 *
 * This is the executor-side counterpart of PipelineFrameContext: the executor
 * knows how many invocations a batch was split into, the pipeline knows the
 * frame.
 */
struct PipelineInvocation {
	UInt32 jobIndex = 0;  ///< Index of this invocation within the batch.
	UInt32 jobCount = 1;  ///< Number of invocations in the batch.
};

/**
 * @brief Snapshot of one task's execution, for profiling and tooling.
 */
struct PipelineTaskStats {
	String name;                            ///< Task name.
	PipelineTaskMode mode = PipelineTaskMode::MainThread;
	Subsystem* subsystem = nullptr;         ///< Owning subsystem, or nullptr.
	PipelineTaskStatus status = PipelineTaskStatus::NotRun;
	UInt32 level = 0;                       ///< Dependency-graph level.
	F64 lastMs = 0.0;                       ///< Accumulated execution time of the last run.
	F64 averageMs = 0.0;                    ///< Exponential moving average of lastMs.
	F64 maxMs = 0.0;                        ///< Largest lastMs observed.
	UInt64 runCount = 0;                    ///< Number of runs the task actually executed in.
};

/**
 * @brief Aggregate result of the most recent run().
 */
struct PipelineRunStats {
	UInt64 frameIndex = 0;   ///< Index of the run these numbers describe.
	F64 totalMs = 0.0;       ///< Wall time of the whole run.
	F64 mainThreadMs = 0.0;  ///< Accumulated time of main-thread tasks.
	F64 parallelMs = 0.0;    ///< Accumulated execution time of parallel tasks.
	UInt32 tasksRun = 0;     ///< Tasks that executed.
	UInt32 tasksSkipped = 0; ///< Tasks that were gated out or had a failed dependency.
	UInt32 tasksFailed = 0;  ///< Tasks that returned an error.
	UInt32 levelCount = 0;   ///< Number of dependency-graph levels.
	UInt32 maxLevelWidth = 0;///< Tasks in the widest level (upper bound on useful concurrency).
};

/**
 * @brief Execution-engine contract required by PipelineBase.
 *
 * A conforming executor provides:
 * - `Handle`     - copyable, default-constructible, with `bool isValid() const`.
 * - `Priority`   - the executor's own scheduling priority type (may be trivial).
 * - `TaskDesc`   - a descriptor with the members
 *                  `fn` (callable taking `const PipelineInvocation&` and
 *                  returning `Result<void, CoreError>`), `jobCount`, `groupSize`,
 *                  `priority` and `dependencies` (`Vector<Handle>`).
 * - `dispatch(const TaskDesc&) -> Result<Handle, CoreError>`.
 * - `wait(Handle) -> void`, blocking until the handle is terminal (and helping
 *                  execute pending work where the executor supports it).
 *
 * Errors are reported in the Core error space so the pipeline has a single
 * error type; an executor maps its own error codes onto CoreError.
 */
template <typename T>
concept PipelineExecutor =
	requires {
		typename T::Handle;
		typename T::TaskDesc;
		typename T::Priority;
	} &&
	requires(T& executor, const typename T::TaskDesc& desc, const typename T::Handle& handle) {
		{ handle.isValid() } -> std::convertible_to<bool>;
		{ executor.dispatch(desc) } -> std::same_as<Result<typename T::Handle, CoreError>>;
		{ executor.wait(handle) };
	} &&
	requires(typename T::TaskDesc& desc) {
		desc.fn;
		desc.jobCount;
		desc.groupSize;
		desc.priority;
		desc.dependencies;
	};

/**
 * @brief Executor that runs everything inline, on the calling thread.
 *
 * It needs no threads and no external subsystem, which makes PipelineBase
 * usable with zero dependencies. Parallel tasks are executed as soon as they
 * are dispatched, in the order the pipeline dispatches them, so a pipeline run
 * through this executor is fully deterministic - useful for tests, headless
 * builds, and platforms without threading. Priority and dependencies are
 * accepted and ignored (dependencies are already complete by construction).
 */
class SequentialExecutor {
public:
	/// @brief Opaque completion token. Execution is synchronous, so it only has
	///        to distinguish itself from the invalid handle.
	struct Handle {
		UInt64 id = 0;
		EE_NODISCARD bool isValid() const { return id != 0; }
	};

	/// @brief Scheduling priority; unused by this executor.
	using Priority = int;

	/// @brief A batch of work handed to dispatch().
	struct TaskDesc {
		std::function<Result<void, CoreError>(const PipelineInvocation&)> fn;
		UInt32 jobCount = 1;
		UInt32 groupSize = 1;
		Priority priority = 0;
		Vector<Handle> dependencies;
	};

	/**
	 * @brief Run the batch immediately.
	 * @param desc The work to run.
	 * @return A valid handle, or InvalidArgument if the batch has no function.
	 */
	Result<Handle, CoreError> dispatch(const TaskDesc& desc) {
		if (!desc.fn) return CoreError::InvalidArgument;
		const UInt32 jobCount = (std::max)(1u, desc.jobCount);
		for (UInt32 i = 0; i < jobCount; ++i) {
			PipelineInvocation invocation;
			invocation.jobIndex = i;
			invocation.jobCount = jobCount;
			// The result is reported through the pipeline's own task status, so
			// only a missing function can fail here.
			(void)desc.fn(invocation);
		}
		return Handle{ ++m_nextId };
	}

	/// @brief No-op: dispatch() already ran the work to completion.
	void wait(Handle) {}

private:
	UInt64 m_nextId = 0;
};

/**
 * @brief A named, dependency-ordered execution chain over several subsystems.
 *
 * PipelineBase turns "call these subsystems in this order" into an explicit
 * dependency graph. Each task names the tasks it runs after; the graph is
 * validated once in build() and then executed once per run().
 *
 * The execution engine is a template parameter satisfying PipelineExecutor, so
 * Core stays free of any particular threading subsystem. Jobs::Pipeline is the
 * JobSubsystem-backed alias; PipelineBase<SequentialExecutor> runs entirely on
 * the calling thread.
 *
 * Execution model:
 * - Tasks are grouped into levels: level(t) = 1 + max(level of its
 *   dependencies). All dependencies of a task therefore live in strictly lower
 *   levels.
 * - A task marked MainThread runs inline on the calling thread. Main-thread
 *   tasks run in level order, and in registration order inside a level, so a
 *   main-thread-only pipeline is fully deterministic.
 * - A task marked Parallel is handed to the executor. A task starts only after
 *   its declared dependencies are terminal, which is enforced per task
 *   (main-thread tasks wait for their parallel dependencies; parallel tasks are
 *   dispatched with the handles of the ones still in flight).
 * - Consequently two tasks overlap exactly when neither depends on the other
 *   and at least one of them is Parallel, and only if the executor can actually
 *   run work concurrently.
 *
 * Error handling:
 * - A task that fails marks its transitive dependents Skipped and leaves
 *   independent tasks alone. run() returns the first error in registration
 *   order once everything has settled.
 * - A skipped task is not a failure: its dependents still run.
 *
 * A pipeline owns no resources and does not outlive its executor. It is not
 * reentrant: run() must not be called concurrently with itself, and a Parallel
 * task must not call back into run().
 *
 * @tparam TExecutor An execution engine satisfying PipelineExecutor.
 */
template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
class PipelineBase {
public:
	/// @brief The execution engine this pipeline drives.
	using Executor = TExecutor;
	/// @brief Scheduling priority type of the executor.
	using Priority = typename TExecutor::Priority;

	/**
	 * @brief Description of one task in a pipeline.
	 */
	struct TaskDesc {
		/// Human-readable, unique within the pipeline. Used for logging and lookup.
		String name;

		/// The work. Required.
		PipelineTaskFn fn;

		/// @see PipelineTaskMode. Defaults to the safe, sequential mode.
		PipelineTaskMode mode = PipelineTaskMode::MainThread;

		/**
		 * @brief Tasks that must reach a terminal state before this one starts.
		 *
		 * Dependencies also define the level of the task (level = 1 + max level
		 * of its dependencies), and the level decides when main-thread tasks
		 * run. Two tasks with no dependency between them may run concurrently
		 * when at least one of them is Parallel.
		 */
		Vector<PipelineTaskId> after;

		/**
		 * @brief Optional subsystem this task belongs to.
		 *
		 * When set, the task is skipped while the subsystem is not in the
		 * Running state, which keeps a pipeline that drives many subsystems
		 * from calling into a subsystem that is down. Also used for diagnostics.
		 */
		Subsystem* subsystem = nullptr;

		/// Scheduling priority, interpreted by the executor. Default-initialized.
		Priority priority{};

		/**
		 * @brief Number of invocations for a Parallel task.
		 *
		 * A value greater than 1 turns the task into a parallel-for: `fn` is
		 * invoked jobCount times, each invocation seeing a distinct
		 * PipelineFrameContext::jobIndex. Ignored for main-thread tasks.
		 */
		UInt32 jobCount = 1;

		/// Invocations packed into a single work item (parallel-for only).
		UInt32 groupSize = 1;

		/**
		 * @brief Extra runtime gate, evaluated after the enable flag and the
		 *        subsystem state. Returning false skips the task for this run.
		 *
		 * Dependents of a skipped task still run (a skip is not a failure).
		 */
		PipelineCondition when;

		/// When false the task is skipped. Can be flipped at runtime.
		bool enabled = true;
	};

	/**
	 * @brief Construct a pipeline driven by an executor.
	 * @param executor The execution engine. Must outlive this pipeline and be
	 *                 ready to dispatch before the first run().
	 * @param name Human-readable pipeline name (used for logging).
	 */
	explicit PipelineBase(TExecutor& executor, StringView name = "Pipeline");

	/// @brief Destroy the pipeline. Registered tasks are dropped.
	///
	/// Virtual so that a pipeline can be extended with a subclass (for example
	/// the rendering module's RenderPipeline) and destroyed through the base.
	virtual ~PipelineBase();

	EE_NO_COPY(PipelineBase)
	EE_NO_MOVE(PipelineBase)

	// ------------------------------------------------------------------
	// Construction
	// ------------------------------------------------------------------

	/**
	 * @brief Register a task described by a full descriptor.
	 * @param desc The task description. `name` must be unique and `fn` required.
	 * @return The id of the new task, or InvalidPipelineTaskId if the descriptor
	 *         is unusable (reported in the log).
	 */
	PipelineTaskId addTask(TaskDesc desc);

	/**
	 * @brief Register a task from its individual pieces.
	 * @param name Unique task name.
	 * @param fn The work to perform.
	 * @param mode Execution mode; defaults to main-thread.
	 * @param subsystem Optional owning subsystem (gating + diagnostics).
	 * @return The id of the new task, or InvalidPipelineTaskId on invalid input.
	 */
	PipelineTaskId addTask(StringView name, PipelineTaskFn fn,
		PipelineTaskMode mode = PipelineTaskMode::MainThread,
		Subsystem* subsystem = nullptr);

	/**
	 * @brief Register another pipeline as a single main-thread task.
	 *
	 * The child runs inline (so it cannot overlap with this pipeline's tasks)
	 * but contributes its own tasks to this pipeline's timing. Useful for
	 * structuring a large graph; a child may itself be a PipelineBase subclass.
	 * @param child The pipeline to run. Must outlive this pipeline and share the
	 *              same executor type.
	 * @param after Dependencies of the nesting task.
	 * @param name Task name; defaults to the child's name.
	 * @return The id of the new task, or InvalidPipelineTaskId on invalid input.
	 */
	PipelineTaskId addPipeline(PipelineBase& child, Vector<PipelineTaskId> after = {}, StringView name = {});

	/**
	 * @brief Declare an additional dependency after registration.
	 *
	 * Only valid before build(); declaring dependencies afterwards would
	 * invalidate the computed levels.
	 * @param task The dependent task.
	 * @param dependency The task that must finish first.
	 * @return Result indicating success, or InvalidArgument for bad ids, a
	 *         self-dependency, or a call after build().
	 */
	Result<void, CoreError> dependsOn(PipelineTaskId task, PipelineTaskId dependency);

	/**
	 * @brief Declare several additional dependencies at once.
	 * @param task The dependent task.
	 * @param dependencies The tasks that must finish first.
	 * @return Result indicating success or failure.
	 */
	Result<void, CoreError> dependsOn(PipelineTaskId task, const Vector<PipelineTaskId>& dependencies);

	/**
	 * @brief Validate the graph and compute its levels.
	 *
	 * Checks task names, dependency ids and cycles. Must be called after the
	 * last addTask()/dependsOn() and before the first run(); calling it again
	 * re-validates the current graph.
	 * @return Result indicating success, or InvalidArgument if the graph is
	 *         malformed (the offending tasks are named in the log).
	 */
	Result<void, CoreError> build();

	// ------------------------------------------------------------------
	// Execution
	// ------------------------------------------------------------------

	/**
	 * @brief Execute the pipeline once, managing frame timing internally.
	 * @param deltaTime Seconds since the previous run.
	 * @return Result indicating success, or the first task error.
	 */
	Result<void, CoreError> run(F64 deltaTime);

	/**
	 * @brief Execute the pipeline once with a caller-supplied context.
	 *
	 * frameIndex/elapsedTime are taken from the context as given; use run(dt)
	 * if you want the pipeline to maintain them.
	 * @param context Frame data handed to every task.
	 * @return Result indicating success, or the first task error.
	 */
	Result<void, CoreError> run(const PipelineFrameContext& context);

	// ------------------------------------------------------------------
	// Runtime control
	// ------------------------------------------------------------------

	/// @brief Enable or disable one task (disabled tasks are skipped).
	void setTaskEnabled(PipelineTaskId task, bool enabled);

	/// @return Whether the task is enabled, ignoring subsystem state and gates.
	EE_NODISCARD bool isTaskEnabled(PipelineTaskId task) const;

	/// @brief Enable or disable the whole pipeline (a disabled pipeline is a no-op).
	void setEnabled(bool enabled);

	/// @return Whether the pipeline is enabled.
	EE_NODISCARD bool isEnabled() const;

	/**
	 * @brief Stop running further tasks once any task has failed.
	 *
	 * Defaults to false, which skips only the failed task's dependents. With
	 * parallel tasks the stop is best-effort: work already dispatched still runs.
	 * @param stop True to abort the remainder of a failing run.
	 */
	void setStopOnError(bool stop);

	/// @return The current stop-on-error setting.
	EE_NODISCARD bool stopOnError() const;

	// ------------------------------------------------------------------
	// Queries
	// ------------------------------------------------------------------

	/// @return The pipeline name.
	EE_NODISCARD const String& name() const;

	/// @return Number of registered tasks.
	EE_NODISCARD Size taskCount() const;

	/// @return Whether build() has succeeded for the current graph.
	EE_NODISCARD bool isBuilt() const;

	/**
	 * @brief Look up a task by name.
	 * @param name The task name.
	 * @return The task id, or NullOpt if no task has that name.
	 */
	EE_NODISCARD Optional<PipelineTaskId> findTask(StringView name) const;

	/// @return Per-task statistics from the most recent run.
	EE_NODISCARD const Vector<PipelineTaskStats>& taskStats() const;

	/// @return Aggregate statistics from the most recent run.
	EE_NODISCARD const PipelineRunStats& lastRunStats() const;

	/**
	 * @brief Render the dependency graph as human-readable text.
	 *
	 * One line per task grouped by level, showing mode, owning subsystem,
	 * dependencies and the most recent timing. Intended for logs and tooling.
	 * @return A multi-line description of the pipeline.
	 */
	EE_NODISCARD String describe() const;

	/// @brief Clear per-task timing and the last run statistics.
	void resetStats();

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

// ======================================================================
// Implementation
// ======================================================================

namespace Detail {

	/// @return Seconds elapsed since @p start, as a double.
	inline F64 PipelineSecondsSince(std::chrono::steady_clock::time_point start) {
		return std::chrono::duration<F64>(std::chrono::steady_clock::now() - start).count();
	}

	/// @return Whether a subsystem referenced by a task is currently drivable.
	///
	/// Only a subsystem in the Running state is called into: one that was never
	/// initialized, was shut down, or crashed must not be touched. A task with
	/// no subsystem is always drivable.
	inline bool IsSubsystemDrivable(const Subsystem* subsystem) {
		if (!subsystem) return true;
		return subsystem->state() == SubsystemState::Running;
	}

} // namespace Detail

/// @brief Implementation state of a PipelineBase.
template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
struct PipelineBase<TExecutor>::Impl {
	TExecutor* executor = nullptr;
	String name;

	/// @brief One registered task plus everything derived from the graph.
	struct Task {
		TaskDesc desc;
		UInt32 level = 0;
		/// Indices of dependencies that run as jobs. A main-thread task has to
		/// wait for these explicitly; the executor's dependency list covers the
		/// rest.
		Vector<Size> parallelDeps;
	};

	/// @brief Per-task state an executor may touch from another thread. Atomics
	///        only, so the array can be shared without a lock.
	struct Shared {
		std::atomic<PipelineTaskStatus> status{ PipelineTaskStatus::NotRun };
		std::atomic<UInt64> elapsedNs{ 0 };
		std::atomic<CoreError> error{ CoreError::None };
	};

	/// @brief Per-task state only ever touched by the run() thread.
	struct Runtime {
		typename TExecutor::Handle handle{};
		F64 lastMs = 0.0;
		F64 averageMs = 0.0;
		F64 maxMs = 0.0;
		UInt64 runCount = 0;
	};

	Vector<Task> tasks;
	Vector<Vector<Size>> levels;   ///< Task indices per level, ascending.
	Size maxLevelWidth = 0;

	/// Allocated once in build(); std::atomic is neither copyable nor movable,
	/// so this cannot live in a std::vector.
	Uptr<Shared[]> shared;
	Vector<Runtime> runtime;

	Vector<typename TExecutor::Handle> dispatched;  ///< Handles of the current run.
	Vector<typename TExecutor::Handle> depScratch;  ///< Scratch: dependencies of one dispatch.

	Vector<PipelineTaskStats> stats;
	PipelineRunStats lastRun;

	bool built = false;
	bool enabled = true;
	bool stopOnError = false;
	bool running = false;
	UInt64 frameIndex = 0;
	F64 elapsedTime = 0.0;

	// ------------------------------------------------------------------
	// Construction helpers
	// ------------------------------------------------------------------

	/// @return The index of the task with this name, or NullOpt.
	Optional<Size> findTaskIndex(StringView taskName) const {
		for (Size i = 0; i < tasks.size(); ++i) {
			if (tasks[i].desc.name == taskName) return i;
		}
		return NullOpt;
	}

	/// @brief Append a task, checking the descriptor first.
	PipelineTaskId pushTask(TaskDesc desc) {
		if (desc.fn == nullptr) {
			EError("Pipeline '{}': task '{}' has no function - rejected.", name, desc.name);
			return InvalidPipelineTaskId;
		}
		if (desc.name.empty()) {
			EError("Pipeline '{}': a task with an empty name was rejected.", name);
			return InvalidPipelineTaskId;
		}
		if (findTaskIndex(desc.name).has_value()) {
			EError("Pipeline '{}': duplicate task name '{}' - rejected.", name, desc.name);
			return InvalidPipelineTaskId;
		}
		if (desc.mode == PipelineTaskMode::Parallel && (desc.jobCount == 0 || desc.groupSize == 0)) {
			EWarn("Pipeline '{}': task '{}' has jobCount/groupSize 0; clamped to 1.", name, desc.name);
			desc.jobCount = 1;
			desc.groupSize = 1;
		}
		if (built) {
			// The derived arrays (levels, shared state, statistics) are now stale.
			built = false;
			EWarn("Pipeline '{}': task '{}' added after build(); call build() again before run().", name, desc.name);
		}

		Task task;
		task.desc = std::move(desc);
		tasks.push_back(std::move(task));
		return PipelineTaskId{ static_cast<UInt32>(tasks.size() - 1) };
	}

	// ------------------------------------------------------------------
	// Execution helpers
	// ------------------------------------------------------------------

	/// @brief Transition a task to Skipped unless a job already concluded it.
	void markSkipped(Size index) {
		PipelineTaskStatus expected = PipelineTaskStatus::NotRun;
		shared[index].status.compare_exchange_strong(expected, PipelineTaskStatus::Skipped,
			std::memory_order_release, std::memory_order_relaxed);
	}

	/// @return Whether every dependency reached a non-failed terminal state.
	///         Only meaningful once the dependencies have stopped running.
	bool dependenciesSucceeded(Size index) const {
		for (PipelineTaskId dep : tasks[index].desc.after) {
			if (shared[dep.index].status.load(std::memory_order_acquire) == PipelineTaskStatus::Failed) return false;
		}
		return true;
	}

	/// @return Whether the task passes its own gates, ignoring dependencies.
	bool isRunnable(Size index, const PipelineFrameContext& context) const {
		const TaskDesc& desc = tasks[index].desc;
		if (!desc.enabled) return false;
		if (!Detail::IsSubsystemDrivable(desc.subsystem)) return false;
		if (desc.when && !desc.when(context)) return false;
		return true;
	}

	/// @return true if any task has failed in the current run.
	bool anyFailed() const {
		for (Size i = 0; i < tasks.size(); ++i) {
			if (shared[i].status.load(std::memory_order_acquire) == PipelineTaskStatus::Failed) return true;
		}
		return false;
	}

	/// @brief Fold this run's raw counters into the statistics snapshot.
	void finalizeRun(const PipelineFrameContext& context, F64 totalMs) {
		PipelineRunStats run;
		run.frameIndex = context.frameIndex;
		run.totalMs = totalMs;
		run.levelCount = static_cast<UInt32>(levels.size());
		run.maxLevelWidth = static_cast<UInt32>(maxLevelWidth);

		for (Size i = 0; i < tasks.size(); ++i) {
			const PipelineTaskStatus status = shared[i].status.load(std::memory_order_acquire);
			Runtime& rt = runtime[i];
			PipelineTaskStats& s = stats[i];

			s.status = status;
			if (status == PipelineTaskStatus::Succeeded || status == PipelineTaskStatus::Failed) {
				rt.lastMs = static_cast<F64>(shared[i].elapsedNs.load(std::memory_order_relaxed)) / 1e6;
				rt.maxMs = (std::max)(rt.maxMs, rt.lastMs);
				// Exponential moving average; the first sample seeds it.
				rt.averageMs = (rt.runCount == 0) ? rt.lastMs : (rt.averageMs * 0.9 + rt.lastMs * 0.1);
				++rt.runCount;

				if (tasks[i].desc.mode == PipelineTaskMode::Parallel) run.parallelMs += rt.lastMs;
				else run.mainThreadMs += rt.lastMs;
				++run.tasksRun;
				if (status == PipelineTaskStatus::Failed) ++run.tasksFailed;
			} else {
				rt.lastMs = 0.0;
				++run.tasksSkipped;
			}

			s.lastMs = rt.lastMs;
			s.averageMs = rt.averageMs;
			s.maxMs = rt.maxMs;
			s.runCount = rt.runCount;
		}

		lastRun = run;
	}

	/// @brief Time one task invocation and accumulate its duration.
	Result<void, CoreError> invoke(Size index, const PipelineFrameContext& context) {
		const auto start = std::chrono::steady_clock::now();
		const Result<void, CoreError> result = tasks[index].desc.fn(context);
		const UInt64 elapsedNs = static_cast<UInt64>(Detail::PipelineSecondsSince(start) * 1e9);
		shared[index].elapsedNs.fetch_add(elapsedNs, std::memory_order_relaxed);
		return result;
	}
};

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
PipelineBase<TExecutor>::PipelineBase(TExecutor& executor, StringView name) : m_impl(std::make_unique<Impl>()) {
	m_impl->executor = &executor;
	m_impl->name = String(name);
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
PipelineBase<TExecutor>::~PipelineBase() = default;

// ----------------------------------------------------------------------
// Construction
// ----------------------------------------------------------------------

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
PipelineTaskId PipelineBase<TExecutor>::addTask(TaskDesc desc) {
	return m_impl->pushTask(std::move(desc));
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
PipelineTaskId PipelineBase<TExecutor>::addTask(StringView name, PipelineTaskFn fn, PipelineTaskMode mode, Subsystem* subsystem) {
	TaskDesc desc;
	desc.name = String(name);
	desc.fn = std::move(fn);
	desc.mode = mode;
	desc.subsystem = subsystem;
	return m_impl->pushTask(std::move(desc));
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
PipelineTaskId PipelineBase<TExecutor>::addPipeline(PipelineBase& child, Vector<PipelineTaskId> after, StringView name) {
	TaskDesc desc;
	desc.name = name.empty() ? child.name() : String(name);
	desc.mode = PipelineTaskMode::MainThread;
	desc.after = std::move(after);
	PipelineBase* const childPtr = &child;
	desc.fn = [childPtr](const PipelineFrameContext& context) -> Result<void, CoreError> {
		return childPtr->run(context);
	};
	return m_impl->pushTask(std::move(desc));
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
Result<void, CoreError> PipelineBase<TExecutor>::dependsOn(PipelineTaskId task, PipelineTaskId dependency) {
	return dependsOn(task, Vector<PipelineTaskId>{ dependency });
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
Result<void, CoreError> PipelineBase<TExecutor>::dependsOn(PipelineTaskId task, const Vector<PipelineTaskId>& dependencies) {
	auto& p = *m_impl;
	if (p.built) {
		EError("Pipeline '{}': dependsOn() after build() is not allowed.", p.name);
		return CoreError::InvalidArgument;
	}
	if (task.index >= p.tasks.size()) return CoreError::InvalidArgument;

	for (PipelineTaskId dep : dependencies) {
		if (!dep.isValid() || dep.index >= p.tasks.size()) {
			EError("Pipeline '{}': task '{}' depends on an unknown task id {}.",
				p.name, p.tasks[task.index].desc.name, dep.index);
			return CoreError::InvalidArgument;
		}
		if (dep.index == task.index) {
			EError("Pipeline '{}': task '{}' cannot depend on itself.", p.name, p.tasks[task.index].desc.name);
			return CoreError::InvalidArgument;
		}
		p.tasks[task.index].desc.after.push_back(dep);
	}
	return {};
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
Result<void, CoreError> PipelineBase<TExecutor>::build() {
	auto& p = *m_impl;

	if (p.tasks.empty()) {
		EError("Pipeline '{}': build() with no tasks.", p.name);
		return CoreError::InvalidArgument;
	}

	const Size taskCount = p.tasks.size();

	// --- validate dependency ids and self-dependencies ---
	for (Size i = 0; i < taskCount; ++i) {
		for (PipelineTaskId dep : p.tasks[i].desc.after) {
			if (!dep.isValid() || dep.index >= taskCount) {
				EError("Pipeline '{}': task '{}' depends on an unknown task id {}.",
					p.name, p.tasks[i].desc.name, dep.index);
				return CoreError::InvalidArgument;
			}
			if (dep.index == i) {
				EError("Pipeline '{}': task '{}' cannot depend on itself.", p.name, p.tasks[i].desc.name);
				return CoreError::InvalidArgument;
			}
		}
	}

	// --- resolve levels (longest path from a source).
	// Tasks that never become ready are part of a cycle.
	Vector<bool> resolved(taskCount, false);
	Size resolvedCount = 0;
	for (Size pass = 0; pass < taskCount; ++pass) {
		bool progress = false;
		for (Size i = 0; i < taskCount; ++i) {
			if (resolved[i]) continue;
			UInt32 level = 0;
			bool ready = true;
			for (PipelineTaskId dep : p.tasks[i].desc.after) {
				if (!resolved[dep.index]) { ready = false; break; }
				level = (std::max)(level, p.tasks[dep.index].level + 1u);
			}
			if (!ready) continue;
			p.tasks[i].level = level;
			resolved[i] = true;
			++resolvedCount;
			progress = true;
		}
		if (resolvedCount == taskCount) break;
		if (!progress) {
			String cycle;
			for (Size i = 0; i < taskCount; ++i) {
				if (resolved[i]) continue;
				if (!cycle.empty()) cycle += ", ";
				cycle += p.tasks[i].desc.name;
			}
			EError("Pipeline '{}': dependency cycle between task(s): {}.", p.name, cycle);
			return CoreError::InvalidArgument;
		}
	}

	// --- group tasks by level ---
	UInt32 levelCount = 0;
	for (Size i = 0; i < taskCount; ++i) levelCount = (std::max)(levelCount, p.tasks[i].level + 1u);
	p.levels.assign(levelCount, {});
	p.maxLevelWidth = 0;
	for (Size i = 0; i < taskCount; ++i) p.levels[p.tasks[i].level].push_back(i);
	for (const auto& level : p.levels) p.maxLevelWidth = (std::max)(p.maxLevelWidth, level.size());

	// --- per-task derived data ---
	for (Size i = 0; i < taskCount; ++i) {
		auto& parallelDeps = p.tasks[i].parallelDeps;
		parallelDeps.clear();
		for (PipelineTaskId dep : p.tasks[i].desc.after) {
			if (p.tasks[dep.index].desc.mode == PipelineTaskMode::Parallel) parallelDeps.push_back(dep.index);
		}
	}

	p.shared = std::make_unique<typename Impl::Shared[]>(taskCount);
	p.runtime.assign(taskCount, typename Impl::Runtime{});
	p.dispatched.clear();
	p.dispatched.reserve(taskCount);

	// --- statistics view (kept index-aligned with tasks) ---
	p.stats.clear();
	p.stats.reserve(taskCount);
	for (Size i = 0; i < taskCount; ++i) {
		PipelineTaskStats s;
		s.name = p.tasks[i].desc.name;
		s.mode = p.tasks[i].desc.mode;
		s.subsystem = p.tasks[i].desc.subsystem;
		s.level = p.tasks[i].level;
		p.stats.push_back(std::move(s));
	}

	p.built = true;

	Size parallelCount = 0;
	for (const auto& task : p.tasks) {
		if (task.desc.mode == PipelineTaskMode::Parallel) ++parallelCount;
	}
	EInfo("Pipeline '{}' built: {} task(s), {} level(s), widest level {} task(s), {} parallel.",
		p.name, taskCount, p.levels.size(), p.maxLevelWidth, parallelCount);

	return {};
}

// ----------------------------------------------------------------------
// Execution
// ----------------------------------------------------------------------

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
Result<void, CoreError> PipelineBase<TExecutor>::run(F64 deltaTime) {
	PipelineFrameContext context;
	context.frameIndex = m_impl->frameIndex++;
	context.deltaTime = deltaTime;
	m_impl->elapsedTime += deltaTime;
	context.elapsedTime = m_impl->elapsedTime;
	return run(context);
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
Result<void, CoreError> PipelineBase<TExecutor>::run(const PipelineFrameContext& contextIn) {
	auto& p = *m_impl;

	if (!p.enabled) return {};
	if (!p.built) {
		EError("Pipeline '{}': run() called before build().", p.name);
		return CoreError::NotInitialized;
	}
	if (p.running) {
		EError("Pipeline '{}': run() is not reentrant (a task called back into the pipeline?).", p.name);
		return CoreError::ThreadingError;
	}

	/// Clears the reentrancy flag on every exit path.
	struct RunGuard {
		bool& flag;
		~RunGuard() { flag = false; }
	} guard{ p.running };
	p.running = true;

	const auto runStart = std::chrono::steady_clock::now();
	const Size taskCount = p.tasks.size();

	// --- reset per-run state ---
	// Safe without synchronization: the previous run waited for every job it
	// dispatched before returning.
	for (Size i = 0; i < taskCount; ++i) {
		p.shared[i].status.store(PipelineTaskStatus::NotRun, std::memory_order_relaxed);
		p.shared[i].elapsedNs.store(0, std::memory_order_relaxed);
		p.shared[i].error.store(CoreError::None, std::memory_order_relaxed);
		p.runtime[i].handle = typename TExecutor::Handle{};
	}
	p.dispatched.clear();

	const PipelineFrameContext context = contextIn;
	Impl* const impl = &p;

	// --- level-ordered execution ---
	// Dependencies always live in strictly lower levels, so a task's
	// dependencies are already terminal by the time its level is reached.
	for (const auto& level : p.levels) {
		// Stop-on-error is best-effort: a parallel failure may not be visible
		// until the level boundary.
		const bool aborting = p.stopOnError && p.anyFailed();

		// 1) Dispatch this level's parallel tasks.
		for (Size i : level) {
			if (p.tasks[i].desc.mode != PipelineTaskMode::Parallel) continue;

			if (!p.isRunnable(i, context) || aborting || !p.dependenciesSucceeded(i)) {
				p.markSkipped(i);
				continue;
			}

			const PipelineFrameContext taskContext = context;
			typename TExecutor::TaskDesc execDesc;
			execDesc.jobCount = (std::max)(1u, p.tasks[i].desc.jobCount);
			execDesc.groupSize = (std::max)(1u, p.tasks[i].desc.groupSize);
			execDesc.priority = p.tasks[i].desc.priority;
			// Queue behind the parallel dependencies that are still in flight.
			// Main-thread dependencies have already run: their level is lower,
			// and main-thread tasks execute level by level on this thread.
			p.depScratch.clear();
			for (Size dep : p.tasks[i].parallelDeps) {
				const typename TExecutor::Handle depHandle = p.runtime[dep].handle;
				if (depHandle.isValid()) p.depScratch.push_back(depHandle);
			}
			execDesc.dependencies = p.depScratch;
			execDesc.fn = [impl, i, taskContext](const PipelineInvocation& invocation) -> Result<void, CoreError> {
				// Dependencies are terminal before the job is queued, so a
				// failed dependency is observable here.
				if (!impl->dependenciesSucceeded(i)) {
					impl->markSkipped(i);
					return {};
				}

				PipelineFrameContext local = taskContext;
				local.jobIndex = invocation.jobIndex;
				local.jobCount = invocation.jobCount;

				const Result<void, CoreError> result = impl->invoke(i, local);
				if (result.isErr()) {
					// Failure always wins, even if another invocation succeeded.
					impl->shared[i].error.store(result.error(), std::memory_order_relaxed);
					impl->shared[i].status.store(PipelineTaskStatus::Failed, std::memory_order_release);
					return result.error();
				}
				PipelineTaskStatus expected = PipelineTaskStatus::NotRun;
				impl->shared[i].status.compare_exchange_strong(expected, PipelineTaskStatus::Succeeded,
					std::memory_order_release, std::memory_order_relaxed);
				return {};
			};

			const Result<typename TExecutor::Handle, CoreError> handle = p.executor->dispatch(execDesc);
			if (handle.isErr()) {
				EError("Pipeline '{}': failed to dispatch task '{}': {}",
					p.name, p.tasks[i].desc.name, ToString(handle.error()));
				p.shared[i].error.store(handle.error(), std::memory_order_relaxed);
				p.shared[i].status.store(PipelineTaskStatus::Failed, std::memory_order_release);
				continue;
			}
			p.runtime[i].handle = handle.value();
			p.dispatched.push_back(handle.value());
		}

		// 2) Run this level's main-thread tasks, in registration order.
		for (Size i : level) {
			if (p.tasks[i].desc.mode != PipelineTaskMode::MainThread) continue;

			if (!p.isRunnable(i, context)) {
				p.markSkipped(i);
				continue;
			}

			// Wait for the parallel dependencies of *this* task only, so
			// unrelated jobs keep running on the pool. Executors may help
			// execute pending work while waiting instead of only blocking.
			for (Size dep : p.tasks[i].parallelDeps) {
				const typename TExecutor::Handle handle = p.runtime[dep].handle;
				if (handle.isValid()) p.executor->wait(handle);
			}

			if ((p.stopOnError && p.anyFailed()) || !p.dependenciesSucceeded(i)) {
				p.markSkipped(i);
				continue;
			}

			const Result<void, CoreError> result = p.invoke(i, context);
			if (result.isErr()) {
				p.shared[i].error.store(result.error(), std::memory_order_relaxed);
				p.shared[i].status.store(PipelineTaskStatus::Failed, std::memory_order_release);
			} else {
				p.shared[i].status.store(PipelineTaskStatus::Succeeded, std::memory_order_release);
			}
		}
	}

	// --- settle: every job dispatched by this run must be finished ---
	for (const typename TExecutor::Handle handle : p.dispatched) {
		if (handle.isValid()) p.executor->wait(handle);
	}

	p.finalizeRun(context, Detail::PipelineSecondsSince(runStart));

	// Report the first failure in registration order, for determinism.
	if (p.lastRun.tasksFailed > 0) {
		for (Size i = 0; i < taskCount; ++i) {
			if (p.shared[i].status.load(std::memory_order_acquire) == PipelineTaskStatus::Failed) {
				const CoreError error = p.shared[i].error.load(std::memory_order_relaxed);
				EError("Pipeline '{}': task '{}' failed: {}", p.name, p.tasks[i].desc.name, ToString(error));
				return error;
			}
		}
	}
	return {};
}

// ----------------------------------------------------------------------
// Runtime control
// ----------------------------------------------------------------------

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
void PipelineBase<TExecutor>::setTaskEnabled(PipelineTaskId task, bool enabled) {
	auto& p = *m_impl;
	if (task.index >= p.tasks.size()) return;
	p.tasks[task.index].desc.enabled = enabled;
	if (enabled) return;
	// Reflect the change immediately rather than one run later.
	if (task.index < p.stats.size()) p.stats[task.index].status = PipelineTaskStatus::Skipped;
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
bool PipelineBase<TExecutor>::isTaskEnabled(PipelineTaskId task) const {
	const auto& p = *m_impl;
	if (task.index >= p.tasks.size()) return false;
	return p.tasks[task.index].desc.enabled;
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
void PipelineBase<TExecutor>::setEnabled(bool enabled) { m_impl->enabled = enabled; }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
bool PipelineBase<TExecutor>::isEnabled() const { return m_impl->enabled; }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
void PipelineBase<TExecutor>::setStopOnError(bool stop) { m_impl->stopOnError = stop; }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
bool PipelineBase<TExecutor>::stopOnError() const { return m_impl->stopOnError; }

// ----------------------------------------------------------------------
// Queries
// ----------------------------------------------------------------------

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
const String& PipelineBase<TExecutor>::name() const { return m_impl->name; }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
Size PipelineBase<TExecutor>::taskCount() const { return m_impl->tasks.size(); }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
bool PipelineBase<TExecutor>::isBuilt() const { return m_impl->built; }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
Optional<PipelineTaskId> PipelineBase<TExecutor>::findTask(StringView name) const {
	const Optional<Size> index = m_impl->findTaskIndex(name);
	if (!index.has_value()) return NullOpt;
	return PipelineTaskId{ static_cast<UInt32>(index.value()) };
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
const Vector<PipelineTaskStats>& PipelineBase<TExecutor>::taskStats() const { return m_impl->stats; }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
const PipelineRunStats& PipelineBase<TExecutor>::lastRunStats() const { return m_impl->lastRun; }

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
String PipelineBase<TExecutor>::describe() const {
	const auto& p = *m_impl;
	String out;
	out += "Pipeline '" + p.name + "' (" + std::to_string(p.tasks.size()) + " tasks, "
		+ std::to_string(p.levels.size()) + " levels";
	if (p.built) out += ", widest " + std::to_string(p.maxLevelWidth);
	out += ")\n";

	for (Size li = 0; li < p.levels.size(); ++li) {
		out += "  level " + std::to_string(li) + ":\n";
		for (Size i : p.levels[li]) {
			const typename Impl::Task& task = p.tasks[i];
			out += "    ";
			out += (task.desc.mode == PipelineTaskMode::Parallel) ? "[parallel] " : "[main]     ";
			out += task.desc.name;
			if (task.desc.subsystem) out += "  (" + task.desc.subsystem->name() + ")";
			if (task.desc.jobCount > 1) out += "  jobs=" + std::to_string(task.desc.jobCount);
			if (!task.desc.after.empty()) {
				out += "  after=";
				for (Size d = 0; d < task.desc.after.size(); ++d) {
					if (d) out += ",";
					out += p.tasks[task.desc.after[d].index].desc.name;
				}
			}
			if (i < p.stats.size() && p.runtime[i].runCount > 0) {
				out += "  [" + String(ToString(p.stats[i].status)) + " "
					+ std::to_string(p.stats[i].lastMs) + "ms]";
			}
			if (!task.desc.enabled) out += "  (disabled)";
			out += "\n";
		}
	}
	return out;
}

template <typename TExecutor>
	requires PipelineExecutor<TExecutor>
void PipelineBase<TExecutor>::resetStats() {
	auto& p = *m_impl;
	for (auto& rt : p.runtime) rt = typename Impl::Runtime{};
	for (auto& s : p.stats) {
		s.status = PipelineTaskStatus::NotRun;
		s.lastMs = 0.0;
		s.averageMs = 0.0;
		s.maxMs = 0.0;
		s.runCount = 0;
	}
	p.lastRun = PipelineRunStats{};
}

EE_NAMESPACE_END
