#pragma once

#include <Engine/Core/PipelineBase.hpp>
#include <Engine/Jobs/JobSubsystem.hpp>
#include <Engine/Jobs/JobTypes.hpp>

EE_NAMESPACE_JOBS_BEGIN

/**
 * @brief Map a JobError onto the Core error space.
 *
 * PipelineBase speaks CoreError only, so an executor has to translate. The
 * precise job error is logged at the dispatch site before it is mapped.
 * @param error The job error.
 * @return The equivalent core error.
 */
inline CoreError ToCoreError(JobError error) {
	switch (error) {
	case JobError::None: return CoreError::None;
	case JobError::NotInitialized: return CoreError::NotInitialized;
	case JobError::AlreadyInitialized: return CoreError::AlreadyInitialized;
	case JobError::InvalidArgument: return CoreError::InvalidArgument;
	case JobError::InvalidHandle: return CoreError::InvalidArgument;
	case JobError::DependencyNotFound: return CoreError::InvalidArgument;
	case JobError::ThreadCreationFailed: return CoreError::ThreadingError;
	case JobError::JobCrashed: return CoreError::SubsystemCrashed;
	case JobError::JobCancelled: return CoreError::OperationFailed;
	default: return CoreError::OperationFailed;
	}
}

/**
 * @brief Pipeline executor backed by the JobSubsystem worker pool.
 *
 * Satisfies the PipelineExecutor contract: batches are dispatched to the job
 * system and waited on through it. Because JobSubsystem::wait() helps execute
 * pending work, a main-thread task blocking on a parallel dependency keeps the
 * pool busy instead of idling.
 *
 * The executor holds a reference; the JobSubsystem must outlive it and must be
 * initialized before the first dispatch.
 */
class JobExecutor {
public:
	/// @brief Batch handle, as produced by the job system.
	using Handle = JobHandle;
	/// @brief Scheduling priority understood by the job system.
	using Priority = JobPriority;

	/// @brief A batch of work handed to dispatch().
	struct TaskDesc {
		std::function<Result<void, CoreError>(const PipelineInvocation&)> fn;
		UInt32 jobCount = 1;
		UInt32 groupSize = 1;
		Priority priority = Priority::Normal;
		Vector<Handle> dependencies;
	};

	/**
	 * @brief Construct an executor over a job subsystem.
	 * @param jobs The job subsystem to dispatch to. Must outlive this executor.
	 */
	explicit JobExecutor(JobSubsystem& jobs) : m_jobs(&jobs) {}

	/**
	 * @brief Queue a batch on the job system.
	 * @param desc The batch to run.
	 * @return The batch handle, or the mapped job error.
	 */
	Result<Handle, CoreError> dispatch(const TaskDesc& desc) {
		if (!desc.fn) return CoreError::InvalidArgument;

		JobDispatchDesc jobDesc;
		jobDesc.jobCount = (desc.jobCount > 0) ? desc.jobCount : 1u;
		jobDesc.groupSize = (desc.groupSize > 0) ? desc.groupSize : 1u;
		jobDesc.priority = desc.priority;
		jobDesc.dependencies = desc.dependencies;
		// The task reports failure through the pipeline's own status; the job
		// error only has to mark the batch as failed.
		jobDesc.function = [fn = desc.fn](const JobContext& job) -> Result<void, JobError> {
			PipelineInvocation invocation;
			invocation.jobIndex = job.jobIndex;
			invocation.jobCount = job.jobCount;
			const Result<void, CoreError> result = fn(invocation);
			return result.isErr() ? Result<void, JobError>(JobError::OperationFailed) : Result<void, JobError>{};
		};

		auto handle = m_jobs->dispatch(jobDesc);
		if (handle.isErr()) return ToCoreError(handle.error());
		return handle.value();
	}

	/**
	 * @brief Block until a batch reaches a terminal state.
	 *
	 * Helps execute pending work while waiting, so a caller blocked on a
	 * dependency does not leave workers idle.
	 * @param handle The batch to wait for.
	 */
	void wait(Handle handle) {
		if (handle.isValid()) (void)m_jobs->wait(handle);
	}

	/// @return The job subsystem this executor dispatches to.
	EE_NODISCARD JobSubsystem& jobs() const { return *m_jobs; }

private:
	JobSubsystem* m_jobs = nullptr;
};

/// @brief A PipelineBase driven by the JobSubsystem worker pool.
using Pipeline = PipelineBase<JobExecutor>;

#ifndef EE_EXPORTS
// The library provides the single instantiation of the job-backed pipeline (see
// Source/Engine/Jobs/JobExecutor.cpp), so client modules must not instantiate it
// again: doing so would define the same template members in two modules and make
// the linker emit duplicate-symbol errors.
extern template class EE_API PipelineBase<JobExecutor>;
#endif

EE_NAMESPACE_JOBS_END
