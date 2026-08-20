#include <Jobs/JobSubsystem.hpp>
#include <Jobs/JobEvents.hpp>

#include <Core/Log.hpp>
#include <Core/Crash.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <thread>

#ifdef EE_WINDOWS
#include <Windows.h>
#endif

#ifdef EE_LINUX
#include <pthread.h>
#endif

EE_NAMESPACE_JOBS_BEGIN

namespace {

/// @brief Number of consecutive job crashes after which the engine is crashed.
constexpr UInt32 MaxConsecutiveJobCrashes = 16;

/**
 * @brief Get a monotonic timestamp in nanoseconds.
 * @return Nanoseconds on a steady clock.
 */
UInt64 nowNs() {
	return static_cast<UInt64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

/**
 * @brief Set a debug-friendly name for the current thread (best effort).
 * @param name The thread name (UTF-8).
 */
void setCurrentThreadName(const char* name) {
#ifdef EE_WINDOWS
	wchar_t wideName[32];
	Size i = 0;
	for (; i < 31 && name[i] != '\0'; ++i) {
		wideName[i] = static_cast<wchar_t>(name[i]);
	}
	wideName[i] = L'\0';
	SetThreadDescription(GetCurrentThread(), wideName);
#elif defined(EE_LINUX)
	// pthread thread names are limited to 16 bytes including the terminator.
	char shortName[16];
	Size i = 0;
	for (; i < 15 && name[i] != '\0'; ++i) {
		shortName[i] = name[i];
	}
	shortName[i] = '\0';
	pthread_setname_np(pthread_self(), shortName);
#else
	EE_UNUSED(name);
#endif
}

/**
 * @brief Check whether a job state is terminal (Completed/Failed/Cancelled).
 * @param state The state to test.
 * @return true if the state is terminal.
 */
bool isTerminalState(JobState state) {
	return state == JobState::Completed || state == JobState::Failed || state == JobState::Cancelled;
}

/**
 * @brief Execute a job function under the platform crash guard.
 *
 * On Windows, structured exceptions raised by the job are caught so that a
 * crashing job fails its batch instead of taking down the worker thread.
 * C++ object unwinding is not performed in this path, so job functions must
 * not rely on local destructors for correctness when they crash.
 * @param function The job function.
 * @param context The execution context.
 * @return The job result, or JobError::JobCrashed if the job crashed.
 */
Result<void, JobError> executeGuarded(const JobFunction& function, const JobContext& context) {
#ifdef EE_WINDOWS
	Result<void, JobError> result{};
	__try {
		result = function(context);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		result = JobError::JobCrashed;
	}
	return result;
#else
	return function(context);
#endif
}

} // namespace

/**
 * @brief Internal implementation of JobSubsystem (pImpl).
 *
 * Owns the batch pool, work item pool, priority queues and worker threads.
 * Lock ordering is strictly: pool mutex -> queue mutex -> slot wait mutex.
 * No user code (job functions, event handlers) is ever invoked while an
 * internal mutex is held, except for the slot wait mutex during notify.
 */
struct JobSubsystem::Impl {
	/**
	 * @brief A range of job indices belonging to one batch.
	 */
	struct WorkItem {
		UInt32 poolIndex = 0;       ///< Index of this item inside the work item pool.
		UInt32 batchIndex = 0;      ///< Index of the owning batch slot.
		UInt32 firstJobIndex = 0;   ///< First job index (inclusive).
		UInt32 lastJobIndex = 0;    ///< Last job index (exclusive).
	};

	/**
	 * @brief One slot of the batch pool.
	 */
	struct BatchSlot {
		JobFunction function;
		Guid owner;
		Vector<UInt32> dependents;      ///< Batch slots waiting for this batch.
		JobPriority priority = JobPriority::Normal;
		JobError firstError = JobError::None;
		UInt32 totalJobCount = 0;
		UInt32 groupSize = 1;
		std::atomic<UInt32> generation{0};
		std::atomic<JobState> state{JobState::Invalid};
		std::atomic<UInt32> dependencyCount{0};
		std::atomic<UInt32> pendingWorkItems{0};
		std::atomic<bool> finalized{true};
		std::atomic<UInt32> waiterCount{0};
		std::atomic<UInt64> startTimeNs{0};
		std::atomic<UInt64> endTimeNs{0};
		Mutex waitMutex;
		std::condition_variable waitCV;
	};

	explicit Impl(JobSubsystem* owner) : m_owner(owner) {}

	// ------------------------------------------------------------------
	// Lifecycle
	// ------------------------------------------------------------------

	Result<void, JobError> initialize() {
		m_workerCount = m_configWorkerCount;
		if (m_workerCount == 0) {
			UInt32 hardware = std::thread::hardware_concurrency();
			if (hardware == 0) {
				hardware = 4;
			}
			m_workerCount = hardware > 1 ? hardware - 1 : 1;
		}
		if (m_configMaxBatchCount == 0 || m_configMaxWorkItemCount == 0) {
			return JobError::InvalidArgument;
		}

		m_batches.resize(m_configMaxBatchCount);
		m_freeBatchSlots.reserve(m_configMaxBatchCount);
		for (UInt32 i = 0; i < m_configMaxBatchCount; ++i) {
			m_freeBatchSlots.push_back(m_configMaxBatchCount - 1 - i);
		}

		m_workItems.resize(m_configMaxWorkItemCount);
		m_freeWorkItems.reserve(m_configMaxWorkItemCount);
		for (UInt32 i = 0; i < m_configMaxWorkItemCount; ++i) {
			m_workItems[i].poolIndex = i;
			m_freeWorkItems.push_back(m_configMaxWorkItemCount - 1 - i);
		}

		m_stopRequested.store(false, std::memory_order_release);
		m_workers.reserve(m_workerCount);
		for (UInt32 i = 0; i < m_workerCount; ++i) {
			m_workers.emplace_back([this, i] { workerMain(i); });
		}

		EInfo("JobSubsystem '{}': initialized with {} workers, {} batch slots, {} work items",
			m_owner->name(), m_workerCount, m_configMaxBatchCount, m_configMaxWorkItemCount);
		return {};
	}

	void shutdown() {
		// The stop flag must be stored under the queue mutex: otherwise the
		// notify could land between a worker's predicate check and its sleep,
		// losing the wakeup and hanging the join below.
		{
			std::lock_guard<Mutex> queueLock(m_queueMutex);
			m_stopRequested.store(true, std::memory_order_release);
		}
		m_queueCV.notify_all();
		for (std::thread& worker : m_workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}
		m_workers.clear();

		// Force-finalize every batch that did not reach a terminal state.
		// Workers are already joined, so no one else touches the pools.
		UInt64 cancelledCount = 0;
		{
			std::lock_guard<Mutex> poolLock(m_poolMutex);
			for (BatchSlot& slot : m_batches) {
				if (slot.finalized.load(std::memory_order_acquire)) {
					continue;
				}
				slot.state.store(JobState::Cancelled, std::memory_order_release);
				slot.finalized.store(true, std::memory_order_release);
				for (UInt32 dependent : slot.dependents) {
					m_batches[dependent].dependencyCount.fetch_sub(1, std::memory_order_acq_rel);
				}
				{
					std::lock_guard<Mutex> waitLock(slot.waitMutex);
					slot.waitCV.notify_all();
				}
				++cancelledCount;
			}
			for (auto& queue : m_queues) {
				queue.clear();
			}
			m_queuedItemCount.store(0, std::memory_order_release);
			m_freeWorkItems.clear();
			for (UInt32 i = 0; i < m_configMaxWorkItemCount; ++i) {
				m_freeWorkItems.push_back(i);
			}
		}
		m_cancelledBatchCount.fetch_add(cancelledCount, std::memory_order_relaxed);
		m_activeBatchCount.store(0, std::memory_order_release);
		m_workerCount = 0;

		EInfo("JobSubsystem '{}': shut down, {} batches cancelled", m_owner->name(), cancelledCount);
	}

	bool recover() {
		m_consecutiveCrashes.store(0, std::memory_order_release);
		return true;
	}

	// ------------------------------------------------------------------
	// Dispatch
	// ------------------------------------------------------------------

	Result<JobHandle, JobError> dispatch(const JobDispatchDesc& desc) {
		if (m_owner->state() != SubsystemState::Running || m_stopRequested.load(std::memory_order_acquire)) {
			return JobError::NotInitialized;
		}
		if (!desc.function || desc.jobCount == 0 || desc.groupSize == 0) {
			return JobError::InvalidArgument;
		}
		if (!desc.owner.isNil() && !m_owner->hasObject(Object(desc.owner))) {
			return JobError::OwnerNotRegistered;
		}

		UInt32 slotIndex = 0;
		bool needsEnqueue = false;
		{
			std::lock_guard<Mutex> poolLock(m_poolMutex);
			Optional<UInt32> allocated = allocateBatchSlotLocked();
			if (!allocated) {
				return JobError::BatchPoolExhausted;
			}
			slotIndex = *allocated;
			BatchSlot& slot = m_batches[slotIndex];

			for (const JobHandle& dependency : desc.dependencies) {
				if (!isHandleValidLocked(dependency)) {
					releaseBatchSlotLocked(slotIndex);
					return JobError::DependencyNotFound;
				}
			}

			slot.function = desc.function;
			slot.owner = desc.owner;
			slot.priority = desc.priority;
			slot.totalJobCount = desc.jobCount;
			slot.groupSize = desc.groupSize;
			slot.firstError = JobError::None;
			slot.dependents.clear();
			slot.dependencyCount.store(0, std::memory_order_relaxed);
			slot.pendingWorkItems.store(0, std::memory_order_relaxed);
			slot.finalized.store(false, std::memory_order_relaxed);
			slot.waiterCount.store(0, std::memory_order_relaxed);
			slot.startTimeNs.store(0, std::memory_order_relaxed);
			slot.endTimeNs.store(0, std::memory_order_relaxed);

			UInt32 pendingDependencies = 0;
			for (const JobHandle& dependency : desc.dependencies) {
				BatchSlot& dependencySlot = m_batches[dependency.index];
				if (!dependencySlot.finalized.load(std::memory_order_acquire)) {
					dependencySlot.dependents.push_back(slotIndex);
					++pendingDependencies;
				}
			}
			slot.dependencyCount.store(pendingDependencies, std::memory_order_release);
			slot.state.store(pendingDependencies == 0 ? JobState::Pending : JobState::Waiting,
				std::memory_order_release);
			needsEnqueue = pendingDependencies == 0;
		}

		m_submittedBatchCount.fetch_add(1, std::memory_order_relaxed);
		m_activeBatchCount.fetch_add(1, std::memory_order_relaxed);

		if (needsEnqueue) {
			Result<void, JobError> enqueued = enqueueBatch(slotIndex);
			if (enqueued.isErr()) {
				recordFirstError(slotIndex, enqueued.error());
				finalizeBatch(slotIndex, JobState::Failed);
				return enqueued.error();
			}
		}

		JobHandle handle;
		handle.index = slotIndex;
		handle.generation = m_batches[slotIndex].generation.load(std::memory_order_acquire);
		return handle;
	}

	// ------------------------------------------------------------------
	// Wait / cancel / help
	// ------------------------------------------------------------------

	Result<void, JobError> wait(JobHandle handle) {
		return waitInternal(handle, NullOpt);
	}

	Result<void, JobError> waitTimeout(JobHandle handle, F64 timeoutSeconds) {
		if (timeoutSeconds < 0.0) {
			timeoutSeconds = 0.0;
		}
		return waitInternal(handle, timeoutSeconds);
	}

	Result<void, JobError> waitAll(const Vector<JobHandle>& handles) {
		Result<void, JobError> firstError{};
		for (const JobHandle& handle : handles) {
			Result<void, JobError> result = wait(handle);
			if (result.isErr() && firstError.isOk()) {
				firstError = result.error();
			}
		}
		return firstError;
	}

	Result<void, JobError> cancel(JobHandle handle) {
		if (m_owner->state() != SubsystemState::Running) {
			return JobError::NotInitialized;
		}
		bool doFinalize = false;
		{
			std::lock_guard<Mutex> poolLock(m_poolMutex);
			if (!isHandleValidLocked(handle)) {
				return JobError::InvalidHandle;
			}
			BatchSlot& slot = m_batches[handle.index];
			JobState observed = slot.state.load(std::memory_order_acquire);
			bool cancelled = false;
			while (observed == JobState::Waiting || observed == JobState::Pending) {
				JobState from = observed;
				if (slot.state.compare_exchange_weak(observed, JobState::Cancelled,
					std::memory_order_release, std::memory_order_acquire)) {
					cancelled = true;
					// A Waiting batch has no work items yet, so finalize it
					// right away; a Pending batch is finalized by the workers
					// that pop its cancelled work items.
					doFinalize = from == JobState::Waiting;
					break;
				}
			}
			if (!cancelled) {
				if (observed == JobState::Running) {
					EWarn("JobSubsystem '{}': cannot cancel a running batch", m_owner->name());
					return JobError::OperationFailed;
				}
				// Already in a terminal state: cancelling is a no-op.
				return {};
			}
		}
		if (doFinalize) {
			finalizeBatch(handle.index, JobState::Cancelled);
		}
		return {};
	}

	bool helpOnce() {
		if (m_owner->state() != SubsystemState::Running || m_stopRequested.load(std::memory_order_acquire)) {
			return false;
		}
		WorkItem item;
		{
			std::lock_guard<Mutex> queueLock(m_queueMutex);
			if (!tryPopLocked(item)) {
				return false;
			}
			m_queuedItemCount.fetch_sub(1, std::memory_order_acq_rel);
		}
		executeWorkItem(item, InvalidWorkerIndex);
		return true;
	}

	// ------------------------------------------------------------------
	// Queries
	// ------------------------------------------------------------------

	JobState queryState(JobHandle handle) const {
		if (handle.generation == 0 || handle.index >= m_batches.size()) {
			return JobState::Invalid;
		}
		const BatchSlot& slot = m_batches[handle.index];
		JobState state = slot.state.load(std::memory_order_acquire);
		if (slot.generation.load(std::memory_order_acquire) != handle.generation) {
			return JobState::Invalid;
		}
		return state;
	}

	Optional<Guid> queryOwner(JobHandle handle) const {
		if (handle.generation == 0 || handle.index >= m_batches.size()) {
			return NullOpt;
		}
		const BatchSlot& slot = m_batches[handle.index];
		Guid owner = slot.owner;
		if (slot.generation.load(std::memory_order_acquire) != handle.generation) {
			return NullOpt;
		}
		return owner;
	}

	UInt32 workerThreadCount() const {
		return m_workerCount;
	}

	JobSystemStats queryStats() const {
		JobSystemStats stats;
		stats.workerThreadCount = m_workerCount;
		stats.maxBatchCount = m_configMaxBatchCount;
		stats.maxWorkItemCount = m_configMaxWorkItemCount;
		stats.activeBatchCount = m_activeBatchCount.load(std::memory_order_acquire);
		stats.queuedWorkItemCount = m_queuedItemCount.load(std::memory_order_acquire);
		stats.submittedBatchCount = m_submittedBatchCount.load(std::memory_order_relaxed);
		stats.completedBatchCount = m_completedBatchCount.load(std::memory_order_relaxed);
		stats.failedBatchCount = m_failedBatchCount.load(std::memory_order_relaxed);
		stats.cancelledBatchCount = m_cancelledBatchCount.load(std::memory_order_relaxed);
		stats.executedWorkItemCount = m_executedWorkItemCount.load(std::memory_order_relaxed);
		stats.helpExecutedWorkItemCount = m_helpExecutedWorkItemCount.load(std::memory_order_relaxed);
		stats.crashedJobCount = m_crashedJobCount.load(std::memory_order_relaxed);
		{
			std::lock_guard<Mutex> poolLock(m_poolMutex);
			stats.freeBatchSlotCount = static_cast<UInt32>(m_freeBatchSlots.size());
			stats.freeWorkItemCount = static_cast<UInt32>(m_freeWorkItems.size());
		}
		return stats;
	}

	// ------------------------------------------------------------------
	// Worker threads
	// ------------------------------------------------------------------

	void workerMain(UInt32 workerIndex) {
		char threadName[32];
		snprintf(threadName, sizeof(threadName), "EE_JobWorker_%u", workerIndex);
		setCurrentThreadName(threadName);

		while (true) {
			WorkItem item;
			{
				std::unique_lock<Mutex> queueLock(m_queueMutex);
				m_queueCV.wait(queueLock, [this] {
					return m_queuedItemCount.load(std::memory_order_acquire) > 0
						|| m_stopRequested.load(std::memory_order_acquire);
				});
				if (m_stopRequested.load(std::memory_order_acquire)) {
					return;
				}
				if (!tryPopLocked(item)) {
					continue;
				}
				m_queuedItemCount.fetch_sub(1, std::memory_order_acq_rel);
			}
			executeWorkItem(item, workerIndex);
		}
	}

	/**
	 * @brief Pop the highest priority work item. The queue mutex must be held.
	 * @param outItem Receives the popped item.
	 * @return true if an item was popped.
	 */
	bool tryPopLocked(WorkItem& outItem) {
		for (Int32 priority = static_cast<Int32>(JobPriority::Critical);
			priority >= static_cast<Int32>(JobPriority::Low); --priority) {
			auto& queue = m_queues[static_cast<Size>(priority)];
			if (!queue.empty()) {
				outItem = queue.front();
				queue.pop_front();
				return true;
			}
		}
		return false;
	}

	/**
	 * @brief Execute one work item: run its job index range and finish it.
	 * @param item The work item.
	 * @param workerIndex Index of the executing worker, or InvalidWorkerIndex.
	 */
	void executeWorkItem(const WorkItem& item, UInt32 workerIndex) {
		BatchSlot& slot = m_batches[item.batchIndex];

		UInt64 expectedStart = 0;
		UInt64 now = nowNs();
		slot.startTimeNs.compare_exchange_strong(expectedStart, now, std::memory_order_acq_rel);

		JobState expectedState = JobState::Pending;
		slot.state.compare_exchange_strong(expectedState, JobState::Running, std::memory_order_acq_rel);

		if (slot.state.load(std::memory_order_acquire) != JobState::Cancelled) {
			JobContext context;
			context.handle.index = item.batchIndex;
			context.handle.generation = slot.generation.load(std::memory_order_acquire);
			context.jobCount = slot.totalJobCount;
			context.workerIndex = workerIndex;

			for (UInt32 jobIndex = item.firstJobIndex; jobIndex < item.lastJobIndex; ++jobIndex) {
				if (slot.state.load(std::memory_order_acquire) == JobState::Cancelled) {
					break;
				}
				context.jobIndex = jobIndex;
				Result<void, JobError> result = executeGuarded(slot.function, context);
				if (result.isErr()) {
					recordFirstError(item.batchIndex, result.error());
					if (result.error() == JobError::JobCrashed) {
						onJobCrashed();
					}
				}
			}
			m_executedWorkItemCount.fetch_add(1, std::memory_order_relaxed);
			if (workerIndex == InvalidWorkerIndex) {
				m_helpExecutedWorkItemCount.fetch_add(1, std::memory_order_relaxed);
			}
		}

		{
			std::lock_guard<Mutex> poolLock(m_poolMutex);
			m_freeWorkItems.push_back(item.poolIndex);
		}
		UInt32 previous = slot.pendingWorkItems.fetch_sub(1, std::memory_order_acq_rel);
		EAssertD((previous >= 1));
		if (previous == 1) {
			JobState finalState = slot.firstError == JobError::None ? JobState::Completed : JobState::Failed;
			finalizeBatch(item.batchIndex, finalState);
		}
	}

	void onJobCrashed() {
		m_crashedJobCount.fetch_add(1, std::memory_order_relaxed);
		UInt32 consecutive = m_consecutiveCrashes.fetch_add(1, std::memory_order_acq_rel) + 1;
		EError("JobSubsystem '{}': a job crashed during execution (consecutive crashes: {})",
			m_owner->name(), consecutive);
		if (consecutive >= MaxConsecutiveJobCrashes) {
			ECrash("JobSubsystem '%s': %u consecutive job crashes, unable to recover",
				m_owner->name().c_str(), consecutive);
		}
	}

	// ------------------------------------------------------------------
	// Batch bookkeeping
	// ------------------------------------------------------------------

	/**
	 * @brief Enqueue a batch: allocate its work items and push them.
	 * @param slotIndex The batch slot.
	 * @return Success, or WorkItemPoolExhausted.
	 */
	Result<void, JobError> enqueueBatch(UInt32 slotIndex) {
		BatchSlot& slot = m_batches[slotIndex];
		UInt32 itemCount = (slot.totalJobCount + slot.groupSize - 1) / slot.groupSize;

		Vector<WorkItem> items;
		items.reserve(itemCount);
		{
			std::lock_guard<Mutex> poolLock(m_poolMutex);
			if (m_freeWorkItems.size() < itemCount) {
				return JobError::WorkItemPoolExhausted;
			}
			for (UInt32 i = 0; i < itemCount; ++i) {
				UInt32 workItemIndex = m_freeWorkItems.back();
				m_freeWorkItems.pop_back();
				WorkItem& item = m_workItems[workItemIndex];
				item.batchIndex = slotIndex;
				item.firstJobIndex = i * slot.groupSize;
				item.lastJobIndex = Min((i + 1) * slot.groupSize, slot.totalJobCount);
				items.push_back(item);
			}
		}
		slot.pendingWorkItems.store(itemCount, std::memory_order_release);

		{
			std::lock_guard<Mutex> queueLock(m_queueMutex);
			auto& queue = m_queues[static_cast<Size>(slot.priority)];
			for (const WorkItem& item : items) {
				queue.push_back(item);
			}
			m_queuedItemCount.fetch_add(itemCount, std::memory_order_acq_rel);
		}
		if (itemCount > 1) {
			m_queueCV.notify_all();
		} else {
			m_queueCV.notify_one();
		}
		return {};
	}

	/**
	 * @brief Record the first error of a batch (later errors are dropped).
	 * @param slotIndex The batch slot.
	 * @param error The error to record.
	 */
	void recordFirstError(UInt32 slotIndex, JobError error) {
		std::lock_guard<Mutex> poolLock(m_poolMutex);
		BatchSlot& slot = m_batches[slotIndex];
		if (slot.firstError == JobError::None) {
			slot.firstError = error;
		}
	}

	/**
	 * @brief Finalize a batch: release dependents, emit events, publish the
	 * terminal state and wake waiters. Idempotent: the finalized flag
	 * arbitrates concurrent finalizers and only the winner proceeds.
	 *
	 * The terminal state is published exactly once via a CAS loop (Cancelled
	 * always wins over the requested state). Events are emitted before the
	 * state is published, so a waiter that observes the terminal state also
	 * observes all emitted events and the recorded first error.
	 * @param slotIndex The batch slot.
	 * @param requestedState The computed terminal state (Cancelled wins if
	 *        the batch was cancelled concurrently).
	 */
	void finalizeBatch(UInt32 slotIndex, JobState requestedState) {
		BatchSlot& slot = m_batches[slotIndex];
		if (slot.finalized.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		Vector<UInt32> toEnqueue;
		Vector<UInt32> toFinalizeCancelled;
		{
			std::lock_guard<Mutex> poolLock(m_poolMutex);
			for (UInt32 dependent : slot.dependents) {
				BatchSlot& dependentSlot = m_batches[dependent];
				UInt32 previous = dependentSlot.dependencyCount.fetch_sub(1, std::memory_order_acq_rel);
				if (previous == 1) {
					// Serialized with cancel() through the pool mutex.
					JobState dependentState = dependentSlot.state.load(std::memory_order_acquire);
					if (dependentState == JobState::Waiting) {
						dependentSlot.state.store(JobState::Pending, std::memory_order_release);
						toEnqueue.push_back(dependent);
					} else if (dependentState == JobState::Cancelled) {
						toFinalizeCancelled.push_back(dependent);
					}
				}
			}
			m_completedBatchSlots.push_back(slotIndex);
			m_activeBatchCount.fetch_sub(1, std::memory_order_relaxed);
		}

		slot.endTimeNs.store(nowNs(), std::memory_order_release);

		if (!slot.owner.isNil()) {
			JobHandle handle;
			handle.index = slotIndex;
			handle.generation = slot.generation.load(std::memory_order_acquire);
			UInt64 durationNs = slot.endTimeNs.load(std::memory_order_acquire)
				- slot.startTimeNs.load(std::memory_order_acquire);
			JobState observed = slot.state.load(std::memory_order_acquire);
			JobState eventState = observed == JobState::Cancelled ? JobState::Cancelled : requestedState;
			if (eventState == JobState::Completed) {
				m_owner->emit(JobCompletedEvent(handle, slot.owner, durationNs));
			} else if (eventState == JobState::Failed) {
				m_owner->emit(JobFailedEvent(handle, slot.owner, slot.firstError, durationNs));
			}
		}

		// Publish the terminal state exactly once. Cancelled always wins,
		// even if cancel() landed between the event emission and this CAS.
		JobState observed = slot.state.load(std::memory_order_acquire);
		JobState finalState = observed == JobState::Cancelled ? JobState::Cancelled : requestedState;
		while (!slot.state.compare_exchange_weak(observed, finalState,
			std::memory_order_release, std::memory_order_acquire)) {
			if (observed == JobState::Cancelled) {
				finalState = JobState::Cancelled;
			}
		}

		switch (finalState) {
		case JobState::Completed:
			m_completedBatchCount.fetch_add(1, std::memory_order_relaxed);
			m_consecutiveCrashes.store(0, std::memory_order_release);
			break;
		case JobState::Failed:
			m_failedBatchCount.fetch_add(1, std::memory_order_relaxed);
			break;
		case JobState::Cancelled:
			m_cancelledBatchCount.fetch_add(1, std::memory_order_relaxed);
			break;
		default:
			break;
		}

		{
			std::lock_guard<Mutex> waitLock(slot.waitMutex);
			slot.waitCV.notify_all();
		}

		for (UInt32 dependent : toEnqueue) {
			Result<void, JobError> enqueued = enqueueBatch(dependent);
			if (enqueued.isErr()) {
				recordFirstError(dependent, enqueued.error());
				finalizeBatch(dependent, JobState::Failed);
			}
		}
		for (UInt32 dependent : toFinalizeCancelled) {
			finalizeBatch(dependent, JobState::Cancelled);
		}
	}

	// ------------------------------------------------------------------
	// Pool management (pool mutex must be held)
	// ------------------------------------------------------------------

	/**
	 * @brief Allocate a batch slot from the free list or by recycling the
	 * oldest finalized slot without waiters. The pool mutex must be held.
	 * @return The slot index, or NullOpt if the pool is exhausted.
	 */
	Optional<UInt32> allocateBatchSlotLocked() {
		UInt32 index = 0;
		if (!m_freeBatchSlots.empty()) {
			index = m_freeBatchSlots.back();
			m_freeBatchSlots.pop_back();
		} else {
			Optional<UInt32> recycled;
			Size attempts = m_completedBatchSlots.size();
			while (attempts > 0) {
				--attempts;
				UInt32 candidate = m_completedBatchSlots.front();
				m_completedBatchSlots.pop_front();
				BatchSlot& slot = m_batches[candidate];
				bool canRecycle = false;
				{
					std::lock_guard<Mutex> waitLock(slot.waitMutex);
					canRecycle = slot.finalized.load(std::memory_order_acquire)
						&& isTerminalState(slot.state.load(std::memory_order_acquire))
						&& slot.waiterCount.load(std::memory_order_acquire) == 0;
				}
				if (canRecycle) {
					recycled = candidate;
					break;
				}
				m_completedBatchSlots.push_back(candidate);
			}
			if (!recycled) {
				return NullOpt;
			}
			index = *recycled;
		}

		BatchSlot& slot = m_batches[index];
		UInt32 generation = slot.generation.load(std::memory_order_relaxed) + 1;
		if (generation == 0) {
			generation = 1;
		}
		slot.generation.store(generation, std::memory_order_release);
		return index;
	}

	/**
	 * @brief Return a batch slot to the free list without publishing it.
	 * The pool mutex must be held.
	 * @param slotIndex The slot to release.
	 */
	void releaseBatchSlotLocked(UInt32 slotIndex) {
		BatchSlot& slot = m_batches[slotIndex];
		slot.function = nullptr;
		slot.state.store(JobState::Invalid, std::memory_order_release);
		slot.finalized.store(true, std::memory_order_release);
		m_freeBatchSlots.push_back(slotIndex);
	}

	/**
	 * @brief Check a handle against the pool. The pool mutex must be held.
	 * @param handle The handle to validate.
	 * @return true if the handle refers to the current occupant of its slot.
	 */
	bool isHandleValidLocked(JobHandle handle) const {
		if (handle.generation == 0 || handle.index >= m_batches.size()) {
			return false;
		}
		return m_batches[handle.index].generation.load(std::memory_order_acquire) == handle.generation;
	}

	// ------------------------------------------------------------------
	// Waiting
	// ------------------------------------------------------------------

	/**
	 * @brief Shared implementation of wait() and waitTimeout().
	 *
	 * The waiter registers itself so that the slot cannot be recycled while
	 * it waits, and helps execute pending work between wakeups.
	 * @param handle The batch to wait for.
	 * @param timeoutSeconds Optional timeout in seconds.
	 * @return Success if completed, the batch error if failed, JobCancelled
	 *         if cancelled, WaitTimeout on timeout, InvalidHandle if stale.
	 */
	Result<void, JobError> waitInternal(JobHandle handle, Optional<F64> timeoutSeconds) {
		if (m_owner->state() != SubsystemState::Running) {
			return JobError::NotInitialized;
		}
		if (handle.generation == 0 || handle.index >= m_batches.size()) {
			return JobError::InvalidHandle;
		}
		BatchSlot& slot = m_batches[handle.index];

		std::chrono::steady_clock::time_point deadline;
		if (timeoutSeconds) {
			deadline = std::chrono::steady_clock::now()
				+ std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::duration<F64>(*timeoutSeconds));
		}

		std::unique_lock<Mutex> waitLock(slot.waitMutex);
		slot.waiterCount.fetch_add(1, std::memory_order_acq_rel);

		if (slot.generation.load(std::memory_order_acquire) != handle.generation) {
			slot.waiterCount.fetch_sub(1, std::memory_order_acq_rel);
			return JobError::InvalidHandle;
		}

		while (!isTerminalState(slot.state.load(std::memory_order_acquire))) {
			waitLock.unlock();
			bool helped = helpOnce();
			waitLock.lock();
			if (!helped) {
				if (timeoutSeconds) {
					if (!slot.waitCV.wait_until(waitLock, deadline, [&slot] {
							return isTerminalState(slot.state.load(std::memory_order_acquire));
						})) {
						slot.waiterCount.fetch_sub(1, std::memory_order_acq_rel);
						return JobError::WaitTimeout;
					}
				} else {
					slot.waitCV.wait(waitLock, [&slot] {
						return isTerminalState(slot.state.load(std::memory_order_acquire));
					});
				}
			}
		}

		JobState finalState = slot.state.load(std::memory_order_acquire);
		JobError firstError = slot.firstError;
		slot.waiterCount.fetch_sub(1, std::memory_order_acq_rel);

		if (finalState == JobState::Completed) {
			return {};
		}
		if (finalState == JobState::Cancelled) {
			return JobError::JobCancelled;
		}
		return firstError != JobError::None ? firstError : JobError::OperationFailed;
	}

	JobSubsystem* m_owner = nullptr;

	// Configuration (set before initialize()).
	UInt32 m_configWorkerCount = 0;
	UInt32 m_configMaxBatchCount = DefaultMaxBatchCount;
	UInt32 m_configMaxWorkItemCount = DefaultMaxWorkItemCount;

	// Pools (guarded by m_poolMutex for structural changes).
	// std::deque keeps element addresses stable without requiring BatchSlot
	// to be movable (it owns a mutex and a condition variable).
	std::deque<BatchSlot> m_batches;
	Vector<UInt32> m_freeBatchSlots;
	std::deque<UInt32> m_completedBatchSlots;
	Vector<WorkItem> m_workItems;
	Vector<UInt32> m_freeWorkItems;
	mutable Mutex m_poolMutex;

	// Priority queues (guarded by m_queueMutex).
	Array<std::deque<WorkItem>, JobPriorityCount> m_queues;
	Mutex m_queueMutex;
	std::condition_variable m_queueCV;
	std::atomic<UInt32> m_queuedItemCount{0};
	std::atomic<bool> m_stopRequested{false};

	// Workers.
	Vector<std::thread> m_workers;
	UInt32 m_workerCount = 0;

	// Statistics.
	std::atomic<UInt32> m_activeBatchCount{0};
	std::atomic<UInt64> m_submittedBatchCount{0};
	std::atomic<UInt64> m_completedBatchCount{0};
	std::atomic<UInt64> m_failedBatchCount{0};
	std::atomic<UInt64> m_cancelledBatchCount{0};
	std::atomic<UInt64> m_executedWorkItemCount{0};
	std::atomic<UInt64> m_helpExecutedWorkItemCount{0};
	std::atomic<UInt64> m_crashedJobCount{0};
	std::atomic<UInt32> m_consecutiveCrashes{0};
};

// ----------------------------------------------------------------------
// JobSubsystem public API
// ----------------------------------------------------------------------

JobSubsystem::JobSubsystem(StringView name)
	: Subsystem(name), m_impl(std::make_unique<Impl>(this)) {}

JobSubsystem::~JobSubsystem() {
	shutdown();
}

void JobSubsystem::setWorkerThreadCount(UInt32 count) {
	if (state() == SubsystemState::Running) {
		EWarn("JobSubsystem '{}': setWorkerThreadCount() ignored after initialize()", name());
		return;
	}
	m_impl->m_configWorkerCount = count;
}

void JobSubsystem::setMaxBatchCount(UInt32 count) {
	if (state() == SubsystemState::Running) {
		EWarn("JobSubsystem '{}': setMaxBatchCount() ignored after initialize()", name());
		return;
	}
	m_impl->m_configMaxBatchCount = count;
}

void JobSubsystem::setMaxWorkItemCount(UInt32 count) {
	if (state() == SubsystemState::Running) {
		EWarn("JobSubsystem '{}': setMaxWorkItemCount() ignored after initialize()", name());
		return;
	}
	m_impl->m_configMaxWorkItemCount = count;
}

Result<JobHandle, JobError> JobSubsystem::dispatch(const JobDispatchDesc& desc) {
	return m_impl->dispatch(desc);
}

Result<void, JobError> JobSubsystem::wait(JobHandle handle) {
	return m_impl->wait(handle);
}

Result<void, JobError> JobSubsystem::waitTimeout(JobHandle handle, F64 timeoutSeconds) {
	return m_impl->waitTimeout(handle, timeoutSeconds);
}

Result<void, JobError> JobSubsystem::waitAll(const Vector<JobHandle>& handles) {
	return m_impl->waitAll(handles);
}

Result<void, JobError> JobSubsystem::cancel(JobHandle handle) {
	return m_impl->cancel(handle);
}

bool JobSubsystem::helpOnce() {
	return m_impl->helpOnce();
}

JobState JobSubsystem::queryState(JobHandle handle) const {
	return m_impl->queryState(handle);
}

bool JobSubsystem::isComplete(JobHandle handle) const {
	JobState state = m_impl->queryState(handle);
	return state == JobState::Completed || state == JobState::Failed || state == JobState::Cancelled;
}

Optional<Guid> JobSubsystem::queryOwner(JobHandle handle) const {
	return m_impl->queryOwner(handle);
}

UInt32 JobSubsystem::workerThreadCount() const {
	return m_impl->workerThreadCount();
}

JobSystemStats JobSubsystem::queryStats() const {
	return m_impl->queryStats();
}

Result<void, CoreError> JobSubsystem::onInitialize() {
	Result<void, JobError> result = m_impl->initialize();
	if (result.isErr()) {
		EError("JobSubsystem '{}': initialization failed: {}", name(), ToString(result.error()));
		if (result.error() == JobError::InvalidArgument) {
			return CoreError::InvalidArgument;
		}
		return CoreError::ThreadingError;
	}
	return {};
}

void JobSubsystem::onShutdown() {
	m_impl->shutdown();
}

bool JobSubsystem::onRecover() {
	return m_impl->recover();
}

EE_NAMESPACE_JOBS_END
