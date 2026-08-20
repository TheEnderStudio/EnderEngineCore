#pragma once

#include "Errors.hpp"

#include <Engine/Core/Types.hpp>
#include <Engine/Core/Guid.hpp>

#include <functional>

EE_NAMESPACE_JOBS_BEGIN

/**
 * @brief Priority of a dispatched job batch.
 *
 * Workers always drain higher priority queues first. Within one priority
 * level, work items are executed in FIFO order.
 */
enum class JobPriority {
	Low,
	Normal,
	High,
	Critical,
};

/// @brief Number of distinct job priorities.
inline constexpr Size JobPriorityCount = 4;

/**
 * @brief Lifecycle state of a job batch.
 */
enum class JobState {
	Invalid,      ///< The handle is stale or unknown.
	Waiting,      ///< Blocked on dependencies, not yet queued.
	Pending,      ///< Queued, but no work item has started executing.
	Running,      ///< At least one work item is executing.
	Completed,    ///< All work items finished successfully.
	Failed,       ///< At least one work item reported an error.
	Cancelled,    ///< Cancelled before completion.
};

/**
 * @brief Handle identifying a dispatched job batch.
 *
 * Handles are index+generation pairs into the internal batch pool. A handle
 * becomes invalid once its slot is recycled for a newer batch. Always check
 * the result of operations on a handle instead of assuming it is still alive.
 */
struct JobHandle {
	UInt32 index = 0;       ///< Slot index inside the batch pool.
	UInt32 generation = 0;  ///< Generation of the slot; 0 means invalid.

	/// @return true if the handle is not the null handle.
	EE_NODISCARD bool isValid() const { return generation != 0; }

	EE_NODISCARD bool operator==(const JobHandle& other) const {
		return index == other.index && generation == other.generation;
	}

	EE_NODISCARD bool operator!=(const JobHandle& other) const {
		return !(*this == other);
	}
};

/// @brief Null job handle.
inline constexpr JobHandle InvalidJobHandle{};

/// @brief Worker index reported when a job is executed by a non-worker (helping) thread.
inline constexpr UInt32 InvalidWorkerIndex = 0xFFFFFFFFu;

/// @brief Default number of batch slots in the pool.
inline constexpr UInt32 DefaultMaxBatchCount = 4096;

/// @brief Default number of work items in the pool.
inline constexpr UInt32 DefaultMaxWorkItemCount = 16384;

/**
 * @brief Per-execution context passed to every job invocation.
 */
struct JobContext {
	JobHandle handle;                          ///< Handle of the batch this execution belongs to.
	UInt32 jobIndex = 0;                       ///< Index of this execution within the batch, in [0, jobCount).
	UInt32 jobCount = 1;                       ///< Total number of executions in the batch.
	UInt32 workerIndex = InvalidWorkerIndex;   ///< Index of the executing worker, or InvalidWorkerIndex for helping threads.
};

/**
 * @brief Signature of a job function.
 *
 * The function is invoked once per job index in the batch. Return an error
 * JobError to mark the whole batch as failed; the first reported error is
 * kept and delivered through wait() and JobFailedEvent.
 */
using JobFunction = std::function<Result<void, JobError>(const JobContext&)>;

/**
 * @brief Full description of a batch dispatch request.
 *
 * A batch executes `function` once for every job index in [0, jobCount).
 * Executions are packed into work items of at most `groupSize` indices.
 * If `dependencies` is non-empty, the batch is only queued after all of
 * them have reached a terminal state (Completed, Failed, or Cancelled).
 */
struct JobDispatchDesc {
	JobFunction function;                       ///< Required. Invoked once per job index.
	UInt32 jobCount = 1;                        ///< Number of executions in this batch.
	UInt32 groupSize = 1;                       ///< Executions packed into a single work item.
	JobPriority priority = JobPriority::Normal; ///< Scheduling priority.
	Guid owner;                                 ///< Optional owner (nil Guid means no owner and no events).
	Vector<JobHandle> dependencies;             ///< Batches that must finish before this one is queued.
};

/**
 * @brief Snapshot of JobSubsystem runtime statistics.
 *
 * Counters are maintained with atomics; the snapshot is approximate but
 * cheap to query from any thread.
 */
struct JobSystemStats {
	UInt32 workerThreadCount = 0;         ///< Number of worker threads.
	UInt32 maxBatchCount = 0;             ///< Capacity of the batch pool.
	UInt32 maxWorkItemCount = 0;          ///< Capacity of the work item pool.
	UInt32 activeBatchCount = 0;          ///< Batches currently in flight.
	UInt32 queuedWorkItemCount = 0;       ///< Work items waiting in the priority queues.
	UInt32 freeBatchSlotCount = 0;        ///< Batch slots immediately available.
	UInt32 freeWorkItemCount = 0;         ///< Work items immediately available.
	UInt64 submittedBatchCount = 0;       ///< Total batches submitted since initialize().
	UInt64 completedBatchCount = 0;       ///< Total batches completed successfully.
	UInt64 failedBatchCount = 0;          ///< Total batches that reported an error.
	UInt64 cancelledBatchCount = 0;       ///< Total batches cancelled.
	UInt64 executedWorkItemCount = 0;     ///< Total work items executed (including skipped cancelled ones).
	UInt64 helpExecutedWorkItemCount = 0; ///< Work items executed by waiting (helping) threads.
	UInt64 crashedJobCount = 0;           ///< Total job invocations that crashed (SEH guard).
};

EE_NAMESPACE_JOBS_END

namespace std {

template <>
struct hash<EnderEngine::Jobs::JobHandle> {
	size_t operator()(const EnderEngine::Jobs::JobHandle& handle) const {
		return (static_cast<size_t>(handle.generation) << 32u) | static_cast<size_t>(handle.index);
	}
};

} // namespace std
