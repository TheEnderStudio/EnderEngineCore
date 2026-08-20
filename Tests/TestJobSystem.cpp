#include <Engine/Jobs/JobSubsystem.hpp>
#include <Engine/Jobs/JobEvents.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace EnderEngine;
using namespace EnderEngine::Jobs;

namespace {

/// @brief Spin until the flag is set, with a bounded polling interval.
void spinUntil(const std::atomic<bool>& flag) {
	while (!flag.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

} // namespace

TEST(JobSubsystemTest, InitializeAndShutdown) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());
	EXPECT_EQ(jobs.state(), SubsystemState::Running);
	EXPECT_GE(jobs.workerThreadCount(), 1u);
	jobs.shutdown();
	EXPECT_EQ(jobs.state(), SubsystemState::Shutdown);
}

TEST(JobSubsystemTest, DispatchBeforeInitializeFails) {
	JobSubsystem jobs;
	auto result = jobs.dispatch([](const JobContext&) {});
	ASSERT_TRUE(result.isErr());
	EXPECT_EQ(result.error(), JobError::NotInitialized);
}

TEST(JobSubsystemTest, DispatchSingleJob) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	std::atomic<bool> ran{false};
	auto handle = jobs.dispatch([&ran](const JobContext&) {
		ran.store(true, std::memory_order_release);
	});
	ASSERT_TRUE(handle.isOk());
	ASSERT_TRUE(handle.value().isValid());

	EXPECT_TRUE(jobs.wait(handle.value()).isOk());
	EXPECT_TRUE(ran.load(std::memory_order_acquire));
	EXPECT_EQ(jobs.queryState(handle.value()), JobState::Completed);
	EXPECT_TRUE(jobs.isComplete(handle.value()));

	jobs.shutdown();
}

TEST(JobSubsystemTest, DispatchVoidLambda) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	std::atomic<int> counter{0};
	auto handle = jobs.dispatch([&counter](const JobContext&) {
		counter.fetch_add(1, std::memory_order_relaxed);
	});
	ASSERT_TRUE(handle.isOk());
	EXPECT_TRUE(jobs.wait(handle.value()).isOk());
	EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);

	jobs.shutdown();
}

TEST(JobSubsystemTest, DispatchGroupSumsIndices) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	std::atomic<UInt64> sum{0};
	auto handle = jobs.dispatchGroup(1000, [&sum](const JobContext& context) {
		sum.fetch_add(context.jobIndex, std::memory_order_relaxed);
	}, 16);
	ASSERT_TRUE(handle.isOk());
	EXPECT_TRUE(jobs.wait(handle.value()).isOk());
	EXPECT_EQ(sum.load(std::memory_order_relaxed), 1000ull * 999ull / 2ull);

	jobs.shutdown();
}

TEST(JobSubsystemTest, DependenciesRunInOrder) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	Mutex orderMutex;
	Vector<int> order;
	auto first = jobs.dispatch([&](const JobContext&) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		std::lock_guard<Mutex> lock(orderMutex);
		order.push_back(1);
	});
	ASSERT_TRUE(first.isOk());
	auto second = jobs.dispatchAfter({first.value()}, [&](const JobContext&) {
		std::lock_guard<Mutex> lock(orderMutex);
		order.push_back(2);
	});
	ASSERT_TRUE(second.isOk());

	EXPECT_TRUE(jobs.wait(second.value()).isOk());
	ASSERT_EQ(order.size(), 2u);
	EXPECT_EQ(order[0], 1);
	EXPECT_EQ(order[1], 2);

	jobs.shutdown();
}

TEST(JobSubsystemTest, WaitTimeoutOnBlockedBatch) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	std::atomic<bool> blockerStarted{false};
	std::atomic<bool> releaseBlocker{false};
	auto blocker = jobs.dispatch([&](const JobContext&) {
		blockerStarted.store(true, std::memory_order_release);
		while (!releaseBlocker.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	ASSERT_TRUE(blocker.isOk());
	spinUntil(blockerStarted);

	auto dependent = jobs.dispatchAfter({blocker.value()}, [](const JobContext&) {});
	ASSERT_TRUE(dependent.isOk());

	auto timeoutResult = jobs.waitTimeout(dependent.value(), 0.02);
	ASSERT_TRUE(timeoutResult.isErr());
	EXPECT_EQ(timeoutResult.error(), JobError::WaitTimeout);

	releaseBlocker.store(true, std::memory_order_release);
	EXPECT_TRUE(jobs.wait(blocker.value()).isOk());
	EXPECT_TRUE(jobs.wait(dependent.value()).isOk());

	jobs.shutdown();
}

TEST(JobSubsystemTest, InvalidHandleOperations) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	EXPECT_EQ(jobs.queryState(InvalidJobHandle), JobState::Invalid);
	EXPECT_FALSE(jobs.isComplete(InvalidJobHandle));
	EXPECT_FALSE(jobs.queryOwner(InvalidJobHandle).has_value());

	auto waitResult = jobs.wait(InvalidJobHandle);
	ASSERT_TRUE(waitResult.isErr());
	EXPECT_EQ(waitResult.error(), JobError::InvalidHandle);

	auto cancelResult = jobs.cancel(InvalidJobHandle);
	ASSERT_TRUE(cancelResult.isErr());
	EXPECT_EQ(cancelResult.error(), JobError::InvalidHandle);

	jobs.shutdown();
}

TEST(JobSubsystemTest, CancelPendingBatch) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	// Occupy every worker so that the victim stays Pending.
	std::atomic<UInt32> started{0};
	std::atomic<bool> releaseBlockers{false};
	Vector<JobHandle> blockers;
	for (UInt32 i = 0; i < jobs.workerThreadCount(); ++i) {
		auto blocker = jobs.dispatch([&](const JobContext&) {
			started.fetch_add(1, std::memory_order_acq_rel);
			while (!releaseBlockers.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
		ASSERT_TRUE(blocker.isOk());
		blockers.push_back(blocker.value());
	}
	while (started.load(std::memory_order_acquire) < jobs.workerThreadCount()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	std::atomic<bool> victimRan{false};
	auto victim = jobs.dispatch([&](const JobContext&) {
		victimRan.store(true, std::memory_order_release);
	});
	ASSERT_TRUE(victim.isOk());

	EXPECT_TRUE(jobs.cancel(victim.value()).isOk());
	EXPECT_EQ(jobs.queryState(victim.value()), JobState::Cancelled);

	releaseBlockers.store(true, std::memory_order_release);

	auto waitResult = jobs.wait(victim.value());
	ASSERT_TRUE(waitResult.isErr());
	EXPECT_EQ(waitResult.error(), JobError::JobCancelled);
	EXPECT_FALSE(victimRan.load(std::memory_order_acquire));
	EXPECT_TRUE(jobs.waitAll(blockers).isOk());

	jobs.shutdown();
}

TEST(JobSubsystemTest, CancelWaitingBatch) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	std::atomic<bool> blockerStarted{false};
	std::atomic<bool> releaseBlocker{false};
	auto blocker = jobs.dispatch([&](const JobContext&) {
		blockerStarted.store(true, std::memory_order_release);
		while (!releaseBlocker.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	ASSERT_TRUE(blocker.isOk());
	spinUntil(blockerStarted);

	auto waiting = jobs.dispatchAfter({blocker.value()}, [](const JobContext&) {});
	ASSERT_TRUE(waiting.isOk());
	EXPECT_EQ(jobs.queryState(waiting.value()), JobState::Waiting);

	EXPECT_TRUE(jobs.cancel(waiting.value()).isOk());
	EXPECT_EQ(jobs.queryState(waiting.value()), JobState::Cancelled);

	auto waitResult = jobs.wait(waiting.value());
	ASSERT_TRUE(waitResult.isErr());
	EXPECT_EQ(waitResult.error(), JobError::JobCancelled);

	releaseBlocker.store(true, std::memory_order_release);
	EXPECT_TRUE(jobs.wait(blocker.value()).isOk());

	jobs.shutdown();
}

TEST(JobSubsystemTest, BatchPoolExhaustionAndStaleHandle) {
	JobSubsystem jobs;
	jobs.setWorkerThreadCount(1);
	jobs.setMaxBatchCount(2);
	ASSERT_TRUE(jobs.initialize().isOk());

	std::atomic<bool> blockerStarted{false};
	std::atomic<bool> releaseBlocker{false};
	auto blocker = jobs.dispatch([&](const JobContext&) {
		blockerStarted.store(true, std::memory_order_release);
		while (!releaseBlocker.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
	ASSERT_TRUE(blocker.isOk());
	spinUntil(blockerStarted);

	auto waiting = jobs.dispatchAfter({blocker.value()}, [](const JobContext&) {});
	ASSERT_TRUE(waiting.isOk());

	// Both pool slots are occupied; the next dispatch must fail gracefully.
	auto exhausted = jobs.dispatch([](const JobContext&) {});
	ASSERT_TRUE(exhausted.isErr());
	EXPECT_EQ(exhausted.error(), JobError::BatchPoolExhausted);

	releaseBlocker.store(true, std::memory_order_release);
	EXPECT_TRUE(jobs.wait(blocker.value()).isOk());
	EXPECT_TRUE(jobs.wait(waiting.value()).isOk());

	// The pool is full of finalized batches; the next dispatch recycles the
	// oldest slot, invalidating its previous handle.
	auto recycled = jobs.dispatch([](const JobContext&) {});
	ASSERT_TRUE(recycled.isOk());
	EXPECT_TRUE(jobs.wait(recycled.value()).isOk());

	EXPECT_EQ(jobs.queryState(blocker.value()), JobState::Invalid);
	auto staleWait = jobs.wait(blocker.value());
	ASSERT_TRUE(staleWait.isErr());
	EXPECT_EQ(staleWait.error(), JobError::InvalidHandle);

	jobs.shutdown();
}

TEST(JobSubsystemTest, OwnerReceivesCompletedEvent) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	Object owner;
	ASSERT_TRUE(jobs.registerObject(owner).isOk());

	std::atomic<int> eventCount{0};
	std::atomic<bool> ownerMatched{false};
	SubscriptionId subscription = jobs.subscribe<JobCompletedEvent>(owner,
		[&](const JobCompletedEvent& event) {
			eventCount.fetch_add(1, std::memory_order_relaxed);
			if (event.owner == owner.guid()) {
				ownerMatched.store(true, std::memory_order_release);
			}
		});
	ASSERT_NE(subscription, InvalidSubscriptionId);

	auto handle = jobs.dispatchFor(owner, [](const JobContext&) {});
	ASSERT_TRUE(handle.isOk());
	EXPECT_TRUE(jobs.wait(handle.value()).isOk());

	EXPECT_EQ(eventCount.load(std::memory_order_relaxed), 1);
	EXPECT_TRUE(ownerMatched.load(std::memory_order_acquire));

	auto queriedOwner = jobs.queryOwner(handle.value());
	ASSERT_TRUE(queriedOwner.has_value());
	EXPECT_EQ(*queriedOwner, owner.guid());

	jobs.unregisterObject(owner);
	jobs.shutdown();
}

TEST(JobSubsystemTest, OwnerReceivesFailedEvent) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	Object owner;
	ASSERT_TRUE(jobs.registerObject(owner).isOk());

	std::atomic<int> eventCount{0};
	std::atomic<JobError> receivedError{JobError::None};
	SubscriptionId subscription = jobs.subscribe<JobFailedEvent>(owner,
		[&](const JobFailedEvent& event) {
			eventCount.fetch_add(1, std::memory_order_relaxed);
			receivedError.store(event.error, std::memory_order_release);
		});
	ASSERT_NE(subscription, InvalidSubscriptionId);

	auto handle = jobs.dispatchFor(owner, [](const JobContext&) -> Result<void, JobError> {
		return JobError::OperationFailed;
	});
	ASSERT_TRUE(handle.isOk());

	auto waitResult = jobs.wait(handle.value());
	ASSERT_TRUE(waitResult.isErr());
	EXPECT_EQ(waitResult.error(), JobError::OperationFailed);

	EXPECT_EQ(eventCount.load(std::memory_order_relaxed), 1);
	EXPECT_EQ(receivedError.load(std::memory_order_acquire), JobError::OperationFailed);
	EXPECT_EQ(jobs.queryState(handle.value()), JobState::Failed);

	jobs.unregisterObject(owner);
	jobs.shutdown();
}

TEST(JobSubsystemTest, WaitingThreadHelpsExecute) {
	JobSubsystem jobs;
	jobs.setWorkerThreadCount(1);
	ASSERT_TRUE(jobs.initialize().isOk());

	std::atomic<bool> blockerStarted{false};
	std::atomic<bool> releaseBlocker{false};
	std::atomic<bool> blockerDone{false};
	auto blocker = jobs.dispatch([&](const JobContext&) {
		blockerStarted.store(true, std::memory_order_release);
		while (!releaseBlocker.load(std::memory_order_acquire)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		blockerDone.store(true, std::memory_order_release);
	});
	ASSERT_TRUE(blocker.isOk());
	spinUntil(blockerStarted);

	// The only worker is blocked, so wait() must execute this job on the
	// calling (main) thread to make progress.
	std::atomic<bool> quickRan{false};
	auto quick = jobs.dispatch([&](const JobContext&) {
		quickRan.store(true, std::memory_order_release);
	});
	ASSERT_TRUE(quick.isOk());

	EXPECT_TRUE(jobs.wait(quick.value()).isOk());
	EXPECT_TRUE(quickRan.load(std::memory_order_acquire));
	EXPECT_FALSE(blockerDone.load(std::memory_order_acquire));

	releaseBlocker.store(true, std::memory_order_release);
	EXPECT_TRUE(jobs.wait(blocker.value()).isOk());

	jobs.shutdown();
}

TEST(JobSubsystemTest, StatisticsAreTracked) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	Vector<JobHandle> handles;
	for (int i = 0; i < 4; ++i) {
		auto handle = jobs.dispatch([](const JobContext&) {});
		ASSERT_TRUE(handle.isOk());
		handles.push_back(handle.value());
	}
	EXPECT_TRUE(jobs.waitAll(handles).isOk());

	JobSystemStats stats = jobs.queryStats();
	EXPECT_EQ(stats.submittedBatchCount, 4u);
	EXPECT_EQ(stats.completedBatchCount, 4u);
	EXPECT_EQ(stats.failedBatchCount, 0u);
	EXPECT_GE(stats.executedWorkItemCount, 4u);
	EXPECT_GE(stats.workerThreadCount, 1u);
	EXPECT_EQ(stats.maxBatchCount, DefaultMaxBatchCount);
	EXPECT_EQ(stats.maxWorkItemCount, DefaultMaxWorkItemCount);

	jobs.shutdown();
}

TEST(JobSubsystemTest, DefaultWorkerThreadCount) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());
	EXPECT_GE(jobs.workerThreadCount(), 1u);
	jobs.shutdown();
}

TEST(JobSubsystemTest, DispatchWithInvalidArguments) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	JobDispatchDesc desc;
	auto noFunction = jobs.dispatch(desc);
	ASSERT_TRUE(noFunction.isErr());
	EXPECT_EQ(noFunction.error(), JobError::InvalidArgument);

	desc.function = [](const JobContext&) -> Result<void, JobError> { return {}; };
	desc.jobCount = 0;
	auto zeroCount = jobs.dispatch(desc);
	ASSERT_TRUE(zeroCount.isErr());
	EXPECT_EQ(zeroCount.error(), JobError::InvalidArgument);

	jobs.shutdown();
}

TEST(JobSubsystemTest, DispatchForUnregisteredOwnerFails) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	Object owner;
	auto handle = jobs.dispatchFor(owner, [](const JobContext&) {});
	ASSERT_TRUE(handle.isErr());
	EXPECT_EQ(handle.error(), JobError::OwnerNotRegistered);

	jobs.shutdown();
}

#ifdef EE_WINDOWS
TEST(JobSubsystemTest, CrashedJobFailsBatch) {
	JobSubsystem jobs;
	ASSERT_TRUE(jobs.initialize().isOk());

	auto crashed = jobs.dispatch([](const JobContext&) -> Result<void, JobError> {
		*reinterpret_cast<volatile int*>(0) = 42;
		return {};
	});
	ASSERT_TRUE(crashed.isOk());

	auto waitResult = jobs.wait(crashed.value());
	ASSERT_TRUE(waitResult.isErr());
	EXPECT_EQ(waitResult.error(), JobError::JobCrashed);
	EXPECT_GE(jobs.queryStats().crashedJobCount, 1u);

	// The subsystem must stay healthy after a job crash.
	std::atomic<bool> recovered{false};
	auto next = jobs.dispatch([&](const JobContext&) {
		recovered.store(true, std::memory_order_release);
	});
	ASSERT_TRUE(next.isOk());
	EXPECT_TRUE(jobs.wait(next.value()).isOk());
	EXPECT_TRUE(recovered.load(std::memory_order_acquire));

	jobs.shutdown();
}
#endif
