#pragma once

#include <Engine/Core/Types.hpp>

EE_NAMESPACE_JOBS_BEGIN

/**
 * @brief Jobs module error codes.
 */
enum class JobError {
	None,
	NotInitialized,          ///< The JobSubsystem is not initialized or already shut down.
	AlreadyInitialized,      ///< The JobSubsystem has already been initialized.
	InvalidArgument,         ///< A null function, zero count, or otherwise invalid argument was given.
	OwnerNotRegistered,      ///< The owner Object is not registered with the JobSubsystem.
	BatchPoolExhausted,      ///< No free batch slot is available and none can be recycled.
	WorkItemPoolExhausted,   ///< No free work item is available for the batch.
	InvalidHandle,           ///< The JobHandle is null, stale, or out of range.
	DependencyNotFound,      ///< A dependency handle is invalid.
	JobCancelled,            ///< The batch was cancelled before completion.
	JobCrashed,              ///< A job invocation crashed during execution (SEH on Windows).
	WaitTimeout,             ///< A timed wait expired before the batch finished.
	ThreadCreationFailed,    ///< A worker thread could not be created.
	OperationFailed,         ///< Generic operation failure.
};

/**
 * @brief Convert a JobError to a human-readable string.
 * @param error The error code.
 * @return A null-terminated descriptive string.
 */
inline const char* ToString(JobError error) {
	switch (error) {
	case JobError::None: return "None";
	case JobError::NotInitialized: return "NotInitialized";
	case JobError::AlreadyInitialized: return "AlreadyInitialized";
	case JobError::InvalidArgument: return "InvalidArgument";
	case JobError::OwnerNotRegistered: return "OwnerNotRegistered";
	case JobError::BatchPoolExhausted: return "BatchPoolExhausted";
	case JobError::WorkItemPoolExhausted: return "WorkItemPoolExhausted";
	case JobError::InvalidHandle: return "InvalidHandle";
	case JobError::DependencyNotFound: return "DependencyNotFound";
	case JobError::JobCancelled: return "JobCancelled";
	case JobError::JobCrashed: return "JobCrashed";
	case JobError::WaitTimeout: return "WaitTimeout";
	case JobError::ThreadCreationFailed: return "ThreadCreationFailed";
	case JobError::OperationFailed: return "OperationFailed";
	}
	return "Unknown";
}

EE_NAMESPACE_JOBS_END
