#pragma once

#include "JobTypes.hpp"

#include <Engine/Core/Subsystem.hpp>
#include <Engine/Core/Object.hpp>

#include <type_traits>
#include <utility>

EE_NAMESPACE_JOBS_BEGIN

namespace Detail {

/**
 * @brief Wrap a callable into a JobFunction.
 *
 * Callables returning void are wrapped so that they report success; callables
 * already returning Result<void, JobError> are used as-is. The callable must
 * be copy-constructible (a std::function requirement).
 * @tparam F Callable invocable with (const JobContext&).
 * @param function The callable to wrap.
 * @return A JobFunction invoking the callable.
 */
template <typename F>
JobFunction wrapJobFunction(F&& function) {
	if constexpr (std::is_void_v<std::invoke_result_t<F&, const JobContext&>>) {
		return [fn = std::forward<F>(function)](const JobContext& context) mutable -> Result<void, JobError> {
			fn(context);
			return {};
		};
	} else {
		return JobFunction(std::forward<F>(function));
	}
}

} // namespace Detail

/**
 * @brief Subsystem providing parallel job execution on a worker thread pool.
 *
 * JobSubsystem is the entry point of the Jobs module. It owns a fixed pool
 * of batch slots and work items (allocated once during initialize()), a set
 * of priority queues, and a pool of worker threads.
 *
 * Key properties:
 * - All public methods are thread-safe and may be called from any thread.
 * - No heap allocation happens on the dispatch hot path beyond the internal
 *   std::function storage and the dependency list.
 * - Waiting threads help execute pending work instead of only blocking.
 * - Batches may carry an owner Object; completion/failure is reported
 *   through JobCompletedEvent / JobFailedEvent on the subsystem event bus.
 *
 * Lifecycle: construct, optionally configure, initialize(), dispatch jobs,
 * then shutdown() (also performed by the destructor if still running).
 */
class EE_API JobSubsystem : public Subsystem {
public:
	/**
	 * @brief Construct the job subsystem.
	 * @param name Human-readable subsystem name (used for logging).
	 */
	explicit JobSubsystem(StringView name = "JobSubsystem");

	/**
	 * @brief Destroy the subsystem. shutdown() is called if still running.
	 */
	~JobSubsystem() override;

	/**
	 * @brief Set the number of worker threads.
	 *
	 * Must be called before initialize(). A value of 0 selects
	 * max(1, hardware_concurrency - 1) workers automatically.
	 * @param count Desired worker thread count, or 0 for automatic.
	 */
	void setWorkerThreadCount(UInt32 count);

	/**
	 * @brief Set the capacity of the batch pool.
	 *
	 * Must be called before initialize(). When the pool is full, the oldest
	 * finalized batch slot without waiters is recycled; if none can be
	 * recycled, dispatch returns JobError::BatchPoolExhausted.
	 * @param count Maximum number of batches in flight plus retained results.
	 */
	void setMaxBatchCount(UInt32 count);

	/**
	 * @brief Set the capacity of the work item pool.
	 *
	 * Must be called before initialize(). The pool must cover the sum of
	 * ceil(jobCount / groupSize) over all batches queued at the same time.
	 * @param count Maximum number of work items.
	 */
	void setMaxWorkItemCount(UInt32 count);

	/**
	 * @brief Dispatch a batch described by a full descriptor.
	 * @param desc The dispatch descriptor (function is required).
	 * @return The batch handle, or an error (e.g., BatchPoolExhausted).
	 */
	Result<JobHandle, JobError> dispatch(const JobDispatchDesc& desc);

	/**
	 * @brief Dispatch a single fire-and-forget job.
	 * @tparam F Callable with signature void(const JobContext&) or
	 *         Result<void, JobError>(const JobContext&).
	 * @param function The job to execute.
	 * @param priority Scheduling priority.
	 * @return The batch handle, or an error.
	 */
	template <typename F>
		requires std::is_invocable_v<F&, const JobContext&>
	Result<JobHandle, JobError> dispatch(F&& function, JobPriority priority = JobPriority::Normal) {
		JobDispatchDesc desc;
		desc.function = Detail::wrapJobFunction(std::forward<F>(function));
		desc.priority = priority;
		return dispatch(desc);
	}

	/**
	 * @brief Dispatch a single job owned by a registered Object.
	 *
	 * The owner must be registered with this subsystem (registerObject()).
	 * When the batch completes or fails, JobCompletedEvent / JobFailedEvent
	 * is emitted on the subsystem event bus with the owner's Guid.
	 * @tparam F Callable with signature void(const JobContext&) or
	 *         Result<void, JobError>(const JobContext&).
	 * @param owner The owning Object (must be registered).
	 * @param function The job to execute.
	 * @param priority Scheduling priority.
	 * @return The batch handle, or an error (e.g., OwnerNotRegistered).
	 */
	template <typename F>
		requires std::is_invocable_v<F&, const JobContext&>
	Result<JobHandle, JobError> dispatchFor(const Object& owner, F&& function, JobPriority priority = JobPriority::Normal) {
		JobDispatchDesc desc;
		desc.function = Detail::wrapJobFunction(std::forward<F>(function));
		desc.priority = priority;
		desc.owner = owner.guid();
		return dispatch(desc);
	}

	/**
	 * @brief Dispatch a parallel-for batch.
	 *
	 * The function is invoked once per index in [0, jobCount). Indices are
	 * packed into work items of at most groupSize executions each.
	 * @tparam F Callable with signature void(const JobContext&) or
	 *         Result<void, JobError>(const JobContext&).
	 * @param jobCount Number of executions.
	 * @param function The job to execute; use JobContext::jobIndex.
	 * @param groupSize Executions packed into one work item.
	 * @param priority Scheduling priority.
	 * @return The batch handle, or an error.
	 */
	template <typename F>
		requires std::is_invocable_v<F&, const JobContext&>
	Result<JobHandle, JobError> dispatchGroup(UInt32 jobCount, F&& function, UInt32 groupSize = 1, JobPriority priority = JobPriority::Normal) {
		JobDispatchDesc desc;
		desc.function = Detail::wrapJobFunction(std::forward<F>(function));
		desc.priority = priority;
		desc.jobCount = jobCount;
		desc.groupSize = groupSize;
		return dispatch(desc);
	}

	/**
	 * @brief Dispatch a single job that starts only after dependencies finish.
	 *
	 * Dependencies are ordering constraints only: the job runs regardless of
	 * whether dependencies completed, failed, or were cancelled. Use
	 * queryState() on a dependency handle to inspect its outcome.
	 * @tparam F Callable with signature void(const JobContext&) or
	 *         Result<void, JobError>(const JobContext&).
	 * @param dependencies Batch handles that must reach a terminal state first.
	 * @param function The job to execute.
	 * @param priority Scheduling priority.
	 * @return The batch handle, or an error (e.g., DependencyNotFound).
	 */
	template <typename F>
		requires std::is_invocable_v<F&, const JobContext&>
	Result<JobHandle, JobError> dispatchAfter(const Vector<JobHandle>& dependencies, F&& function, JobPriority priority = JobPriority::Normal) {
		JobDispatchDesc desc;
		desc.function = Detail::wrapJobFunction(std::forward<F>(function));
		desc.priority = priority;
		desc.dependencies = dependencies;
		return dispatch(desc);
	}

	/**
	 * @brief Block the calling thread until the batch reaches a terminal state.
	 *
	 * While waiting, the calling thread helps execute pending work items.
	 * @param handle The batch to wait for.
	 * @return Success if the batch completed; the batch's first error if it
	 *         failed; JobCancelled if cancelled; InvalidHandle if stale.
	 */
	Result<void, JobError> wait(JobHandle handle);

	/**
	 * @brief Block the calling thread until the batch finishes or times out.
	 * @param handle The batch to wait for.
	 * @param timeoutSeconds Maximum time to wait, in seconds.
	 * @return Success if the batch completed; the batch error if it failed;
	 *         WaitTimeout if the timeout expired.
	 */
	Result<void, JobError> waitTimeout(JobHandle handle, F64 timeoutSeconds);

	/**
	 * @brief Wait for all given batches.
	 *
	 * Waits for every handle even if some of them fail.
	 * @param handles The batches to wait for.
	 * @return Success if all batches completed; otherwise the first error
	 *         encountered (in the order of the handles).
	 */
	Result<void, JobError> waitAll(const Vector<JobHandle>& handles);

	/**
	 * @brief Cancel a batch that has not started executing yet.
	 *
	 * Cancelling a batch in Waiting or Pending state succeeds immediately.
	 * Cancelling a Running batch fails with OperationFailed; cancelling an
	 * already finished batch is a no-op and succeeds. Dependents are released
	 * as if the batch had finished.
	 * @param handle The batch to cancel.
	 * @return Success, or an error (InvalidHandle, OperationFailed).
	 */
	Result<void, JobError> cancel(JobHandle handle);

	/**
	 * @brief Execute one pending work item on the calling thread, if any.
	 *
	 * Can be used to keep a thread productive while it has nothing to do.
	 * @return true if a work item was executed.
	 */
	bool helpOnce();

	/**
	 * @brief Query the current state of a batch.
	 * @param handle The batch handle.
	 * @return The batch state, or JobState::Invalid for stale handles.
	 */
	EE_NODISCARD JobState queryState(JobHandle handle) const;

	/**
	 * @brief Check whether a batch has reached any terminal state.
	 * @param handle The batch handle.
	 * @return true if the batch is Completed, Failed, or Cancelled.
	 */
	EE_NODISCARD bool isComplete(JobHandle handle) const;

	/**
	 * @brief Query the owner Guid of a batch.
	 * @param handle The batch handle.
	 * @return The owner Guid (possibly nil), or NullOpt for stale handles.
	 */
	EE_NODISCARD Optional<Guid> queryOwner(JobHandle handle) const;

	/// @return The number of worker threads (0 before initialize()).
	EE_NODISCARD UInt32 workerThreadCount() const;

	/// @return A snapshot of runtime statistics.
	EE_NODISCARD JobSystemStats queryStats() const;

protected:
	/// @brief Spawns worker threads and allocates pools.
	Result<void, CoreError> onInitialize() override;

	/// @brief Stops workers, cancels remaining batches, and releases pools.
	void onShutdown() override;

	/// @brief Resets crash counters after a recovery request.
	bool onRecover() override;

private:
	struct Impl;
	Uptr<Impl> m_impl;
};

EE_NAMESPACE_JOBS_END
