#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <gtest/gtest.h>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <spinscale/co/invokers.h>
#include <spinscale/co/nonViralTaskNursery.h>
#include <spinscale/syncCancelerForAsyncWork.h>

namespace {

struct ResumeGate
{
	std::coroutine_handle<> waitingHandle;

	bool await_ready() const noexcept
		{ return false; }

	bool await_suspend(std::coroutine_handle<> callerHandle) noexcept
	{
		waitingHandle = callerHandle;
		return true;
	}

	void await_resume() const noexcept
		{}
};

sscl::co::NonViralNonPostingInvoker immediateCompleteCReq(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;
	co_return;
}

sscl::co::NonViralNonPostingInvoker throwingCompleteCReq(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;
	throw std::runtime_error("nursery test failure");
	co_return;
}

sscl::co::NonViralNonPostingInvoker suspendUntilResumeCReq(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	ResumeGate &gate)
{
	(void)exceptionPtr;
	(void)completion;
	co_await gate;
	co_return;
}

sscl::co::NonViralNonPostingInvoker cancelAwareSuspendCReq(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::SyncCancelerForAsyncWork &canceler,
	ResumeGate &gate)
{
	(void)exceptionPtr;
	(void)completion;

	while (!canceler.isCancellationRequested())
	{
		co_await gate;
	}

	co_return;
}

} // namespace

class NonViralTaskNurseryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		nursery.openAdmission();
	}

	sscl::co::NonViralTaskNursery nursery;
	ResumeGate gate;
	ResumeGate gate2;
};

TEST_F(NonViralTaskNurseryTest, GetNewSlotLeaseFillCommitRetires)
{
	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease]()
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});
	lease.commit();

	EXPECT_TRUE(nursery.allSettled());
	EXPECT_EQ(nursery.unsettledCount(), 0U);
}

TEST_F(NonViralTaskNurseryTest, UncommittedLeaseReleasesReservation)
{
	EXPECT_EQ(nursery.unsettledCount(), 0U);
	{
		auto lease = nursery.getNewSlotLease();
		(void)lease;
	}

	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, CloseAdmissionRejectsNewLeases)
{
	nursery.closeAdmission();

	EXPECT_THROW(nursery.getNewSlotLease(), std::runtime_error);
}

TEST_F(NonViralTaskNurseryTest, SetOnSettledHookRejectsAfterFillSlot)
{
	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease]()
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});

	EXPECT_THROW(
		lease.setOnSettledHook([](std::exception_ptr &) {}),
		std::runtime_error);
	lease.commit();
}

TEST_F(NonViralTaskNurseryTest, AsyncAwaitFiresOnDrain)
{
	std::atomic<bool> drained{false};

	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease]()
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});
	lease.commit();

	nursery.closeAdmission();
	nursery.asyncAwaitAllSettlements(
		[&drained]()
		{
			drained.store(true, std::memory_order_release);
		});

	EXPECT_TRUE(drained.load(std::memory_order_acquire));
}

TEST_F(NonViralTaskNurseryTest, AsyncAwaitRejectsWhenAdmissionOpen)
{
	EXPECT_THROW(nursery.asyncAwaitAllSettlements([]() {}), std::runtime_error);
}

TEST_F(NonViralTaskNurseryTest, SecondDrainWaiterThrows)
{
	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease, this]()
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate);
		});
	lease.commit();

	nursery.closeAdmission();

	bool firstWaiterRegistered = false;
	nursery.asyncAwaitAllSettlements(
		[&firstWaiterRegistered]()
		{
			firstWaiterRegistered = true;
		});

	EXPECT_FALSE(firstWaiterRegistered);
	EXPECT_THROW(
		nursery.asyncAwaitAllSettlements([]() {}),
		std::runtime_error);

	if (gate.waitingHandle)
	{
		gate.waitingHandle.resume();
	}
}

TEST_F(NonViralTaskNurseryTest, SyncAwaitNestedRun)
{
	boost::asio::io_context ioContext;

	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease, this]()
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate);
		});
	lease.commit();

	std::thread awaitThread(
		[this, &ioContext]()
		{
			nursery.closeAdmission();
			nursery.syncAwaitAllSettlements(ioContext);
		});

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	ASSERT_TRUE(static_cast<bool>(gate.waitingHandle));
	gate.waitingHandle.resume();

	awaitThread.join();
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, RequestCancelOnAllDoesNotDestroyInvokers)
{
	auto lease = nursery.getNewSlotLease();
	lease.getSyncCanceler().startAcceptingWork();
	lease.fillSlot(
		[&lease, this]()
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate);
		});
	lease.commit();

	EXPECT_EQ(nursery.unsettledCount(), 1U);
	nursery.requestCancelOnAll();
	EXPECT_EQ(nursery.unsettledCount(), 1U);

	ASSERT_TRUE(static_cast<bool>(gate.waitingHandle));
	gate.waitingHandle.resume();

	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, RequestCancelOnAllStopsCanceler)
{
	auto lease = nursery.getNewSlotLease();
	lease.getSyncCanceler().startAcceptingWork();
	lease.fillSlot(
		[&lease, this]()
		{
			return cancelAwareSuspendCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				lease.getSyncCanceler(),
				gate);
		});
	lease.commit();

	nursery.requestCancelOnAll();
	EXPECT_TRUE(lease.getSyncCanceler().isCancellationRequested());

	ASSERT_TRUE(static_cast<bool>(gate.waitingHandle));
	gate.waitingHandle.resume();

	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, ExceptionPtrRecorded)
{
	std::exception_ptr captured;

	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&captured, &lease]()
		{
			std::exception_ptr &exceptionStorage =
				lease.getExceptionStorage();
			auto invoker = throwingCompleteCReq(
				exceptionStorage,
				lease.getCallerLambda());
			captured = exceptionStorage;
			return invoker;
		});
	lease.commit();

	EXPECT_TRUE(captured != nullptr);
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, LaunchSugar)
{
	auto handle = nursery.launch(
		[](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});

	EXPECT_TRUE(handle == handle);
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, LaunchWithOnSettledHook)
{
	std::atomic<bool> hookRan{false};

	nursery.launch(
		[](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		},
		[&hookRan](std::exception_ptr &)
		{
			hookRan.store(true, std::memory_order_release);
		});

	EXPECT_TRUE(hookRan.load(std::memory_order_acquire));
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, HandleStability)
{
	auto handle = nursery.launch(
		[](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});

	sscl::co::NonViralTaskNursery::Slot::Handle copy = handle;
	EXPECT_TRUE(handle == copy);
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, CommitWithoutFillSlotThrows)
{
	auto lease = nursery.getNewSlotLease();

	EXPECT_THROW(lease.commit(), std::runtime_error);
}

TEST_F(NonViralTaskNurseryTest, DoubleCommitThrows)
{
	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease]()
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});
	lease.commit();

	EXPECT_THROW(lease.commit(), std::runtime_error);
}

TEST_F(NonViralTaskNurseryTest, FillSlotTwiceThrows)
{
	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease, this]()
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate);
		});

	EXPECT_THROW(
		lease.fillSlot(
			[&lease]()
			{
				return immediateCompleteCReq(
					lease.getExceptionStorage(),
					lease.getCallerLambda());
			}),
		std::runtime_error);

	if (gate.waitingHandle) {
		gate.waitingHandle.resume();
	}

	lease.commit();
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, SyncAwaitRejectsWhenAdmissionOpen)
{
	boost::asio::io_context ioContext;

	EXPECT_THROW(
		nursery.syncAwaitAllSettlements(ioContext),
		std::runtime_error);
}

TEST_F(NonViralTaskNurseryTest, SyncAwaitRejectsStoppedIoContext)
{
	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease, this]()
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate);
		});
	lease.commit();

	nursery.closeAdmission();

	boost::asio::io_context ioContext;
	ioContext.stop();

	EXPECT_THROW(
		nursery.syncAwaitAllSettlements(ioContext),
		std::runtime_error);

	if (gate.waitingHandle) {
		gate.waitingHandle.resume();
	}
}

TEST_F(NonViralTaskNurseryTest, SyncAwaitReturnsImmediatelyWhenDrained)
{
	boost::asio::io_context ioContext;

	nursery.closeAdmission();
	EXPECT_TRUE(nursery.allSettled());

	nursery.syncAwaitAllSettlements(ioContext);
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, UnsettledCountTracksInFlightTasks)
{
	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&lease, this]()
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate);
		});
	lease.commit();

	EXPECT_EQ(nursery.unsettledCount(), 1U);
	EXPECT_FALSE(nursery.allSettled());

	if (gate.waitingHandle) {
		gate.waitingHandle.resume();
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_EQ(nursery.unsettledCount(), 0U);
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, MultipleTasksDrainTogether)
{
	std::atomic<bool> drained{false};

	auto lease1 = nursery.getNewSlotLease();
	lease1.fillSlot(
		[&lease1, this]()
		{
			return suspendUntilResumeCReq(
				lease1.getExceptionStorage(),
				lease1.getCallerLambda(),
				gate);
		});
	lease1.commit();

	auto lease2 = nursery.getNewSlotLease();
	lease2.fillSlot(
		[&lease2, this]()
		{
			return suspendUntilResumeCReq(
				lease2.getExceptionStorage(),
				lease2.getCallerLambda(),
				gate2);
		});
	lease2.commit();

	EXPECT_EQ(nursery.unsettledCount(), 2U);

	nursery.closeAdmission();
	nursery.asyncAwaitAllSettlements(
		[&drained]()
		{
			drained.store(true, std::memory_order_release);
		});

	EXPECT_FALSE(drained.load(std::memory_order_acquire));

	if (gate.waitingHandle) {
		gate.waitingHandle.resume();
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_FALSE(drained.load(std::memory_order_acquire));

	if (gate2.waitingHandle) {
		gate2.waitingHandle.resume();
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(drained.load(std::memory_order_acquire));
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, OnSettledHookRunsAtRetirement)
{
	std::atomic<bool> hookRan{false};

	auto lease = nursery.getNewSlotLease();
	lease.setOnSettledHook(
		[&hookRan](std::exception_ptr &)
		{
			hookRan.store(true, std::memory_order_release);
		});
	lease.fillSlot(
		[&lease]()
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});
	lease.commit();

	EXPECT_TRUE(hookRan.load(std::memory_order_acquire));
}

TEST_F(NonViralTaskNurseryTest, OnSettledHookSeesRetiredSlot)
{
	auto lease = nursery.getNewSlotLease();
	lease.setOnSettledHook(
		[this](std::exception_ptr &)
		{
			EXPECT_TRUE(nursery.allSettled());
			EXPECT_EQ(nursery.unsettledCount(), 0U);
		});
	lease.fillSlot(
		[&lease]()
		{
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda());
		});
	lease.commit();
}

TEST_F(NonViralTaskNurseryTest, DuplicateRetireThrows)
{
	std::function<void()> completion;

	auto lease = nursery.getNewSlotLease();
	lease.fillSlot(
		[&completion, &lease]()
		{
			completion = lease.getCallerLambda();
			return immediateCompleteCReq(
				lease.getExceptionStorage(),
				completion);
		});
	lease.commit();

	ASSERT_TRUE(static_cast<bool>(completion));
	EXPECT_THROW(completion(), std::runtime_error);
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, MovedLeaseTransfersReleaseObligation)
{
	EXPECT_EQ(nursery.unsettledCount(), 0U);
	{
		auto lease = nursery.getNewSlotLease();
		auto movedLease = std::move(lease);
		(void)movedLease;
	}

	EXPECT_TRUE(nursery.allSettled());
	EXPECT_EQ(nursery.unsettledCount(), 0U);
}

TEST_F(NonViralTaskNurseryTest, LaunchAssignsDistinctHandles)
{
	auto handle1 = nursery.launch(
		[this](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate);
		});

	auto handle2 = nursery.launch(
		[this](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
		{
			return suspendUntilResumeCReq(
				lease.getExceptionStorage(),
				lease.getCallerLambda(),
				gate2);
		});

	EXPECT_NE(handle1, handle2);
	EXPECT_EQ(nursery.unsettledCount(), 2U);

	if (gate.waitingHandle) {
		gate.waitingHandle.resume();
	}

	if (gate2.waitingHandle) {
		gate2.waitingHandle.resume();
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(nursery.allSettled());
}

TEST_F(NonViralTaskNurseryTest, AdmissionIsOpenReflectsCloseAndOpen)
{
	EXPECT_TRUE(nursery.admissionIsOpen());

	nursery.closeAdmission();
	EXPECT_FALSE(nursery.admissionIsOpen());

	nursery.openAdmission();
	EXPECT_TRUE(nursery.admissionIsOpen());
}
