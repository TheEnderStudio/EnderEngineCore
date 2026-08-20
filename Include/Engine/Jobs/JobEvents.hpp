#pragma once

#include "Errors.hpp"
#include "JobTypes.hpp"

#include <Engine/Core/Event.hpp>
#include <Engine/Core/Guid.hpp>

EE_NAMESPACE_JOBS_BEGIN

/**
 * @brief Emitted when a batch with a registered owner completes successfully.
 *
 * The event is emitted synchronously by JobSubsystem on the thread that
 * finalized the batch (usually a worker thread, possibly a helping thread),
 * so handlers must be thread-safe and should be cheap.
 */
class EE_API JobCompletedEvent : public Event {
public:
	JobCompletedEvent() = default;

	/**
	 * @brief Construct a completion event.
	 * @param handle Handle of the finished batch.
	 * @param owner Guid of the owning Object.
	 * @param durationNs Wall-clock execution duration in nanoseconds.
	 */
	JobCompletedEvent(JobHandle handle, const Guid& owner, UInt64 durationNs)
		: handle(handle), owner(owner), durationNs(durationNs) {}

	JobHandle handle;       ///< Handle of the finished batch (becomes stale once its slot is recycled).
	Guid owner;             ///< Guid of the owning Object.
	UInt64 durationNs = 0;  ///< Wall-clock execution duration in nanoseconds.
};

/**
 * @brief Emitted when a batch with a registered owner fails.
 *
 * A batch fails when at least one of its job invocations returns an error
 * or crashes. Cancelled batches do not emit events. Handlers run on the
 * finalizing thread and must be thread-safe.
 */
class EE_API JobFailedEvent : public Event {
public:
	JobFailedEvent() = default;

	/**
	 * @brief Construct a failure event.
	 * @param handle Handle of the failed batch.
	 * @param owner Guid of the owning Object.
	 * @param error The first error reported by the batch.
	 * @param durationNs Wall-clock execution duration in nanoseconds.
	 */
	JobFailedEvent(JobHandle handle, const Guid& owner, JobError error, UInt64 durationNs)
		: handle(handle), owner(owner), error(error), durationNs(durationNs) {}

	JobHandle handle;              ///< Handle of the failed batch (becomes stale once its slot is recycled).
	Guid owner;                    ///< Guid of the owning Object.
	JobError error = JobError::None; ///< The first error reported by the batch.
	UInt64 durationNs = 0;         ///< Wall-clock execution duration in nanoseconds.
};

EE_NAMESPACE_JOBS_END
