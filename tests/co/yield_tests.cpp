#include <boostAsioLinkageFix.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <spinscale/co/coConditionVariable.h>
#include <spinscale/co/coQutex.h>
#include <spinscale/co/group.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/nonViralTaskNursery.h>
#include <spinscale/co/postTarget.h>
#include <spinscale/co/yield.h>
#include <spinscale/componentThread.h>

#include <support/coroutineDriver.h>
#include <support/groupAssertions.h>
#include <support/threadHarness.h>
#include <support/timerAwaiters.h>

namespace {

constexpr int expectedReturnValue = 42;
constexpr int nestedCalleeReturnValue = 17;
constexpr int nonPostingReturnValue = 9;
constexpr int explicitTargetReturnValue = 77;
constexpr int yieldMemberReturnValue = 11;
constexpr int immediateMemberReturnValue = 13;
constexpr int timerMemberDelayMs = 20;
constexpr int timerThenYieldDelayMs = 20;
constexpr const char *expectedYieldThrowMessage =
	"yield_test intentional failure after yield";
constexpr const char *expectedNurseryThrowMessage =
	"yield_test nursery member failed after yield";
constexpr int eventPostedBeforeYield = 1;
constexpr int eventAfterFirstYield = 2;
constexpr int eventPostedBetweenYields = 3;
constexpr int eventAfterSecondYield = 4;

using CallerDriver =
	sscl::tests::RoleNonViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLER>;

template <typename T>
using CalleeViralInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLEE,
		T>;

class EventLog
{
public:
	void push(int eventId)
	{
		std::lock_guard<std::mutex> guard(mutex);
		events.push_back(eventId);
	}

	std::vector<int> snapshot() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return events;
	}

private:
	mutable std::mutex mutex;
	std::vector<int> events;
};

class ThreadRecorder
{
public:
	void record(std::thread::id &slot)
	{
		std::lock_guard<std::mutex> guard(mutex);
		slot = std::this_thread::get_id();
	}

	std::thread::id read(const std::thread::id &slot) const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return slot;
	}

	mutable std::mutex mutex;
	std::thread::id afterYieldThread;
	std::thread::id calleeThread;
	std::thread::id awaitResumeThread;
};

class QutexOccupancy
{
public:
	std::atomic<bool> holderHolds{false};
	std::atomic<bool> waiterEntered{false};
	std::atomic<bool> holderReleased{false};
};

class YieldInvokerTest
:	public ::testing::Test
{
protected:
	template <typename Factory>
	void runOnCaller(Factory &&factory)
	{
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			std::forward<Factory>(factory));
	}

	sscl::tests::PostingThreadSet threads;
};

class YieldCoreProducers
{
public:
	static CallerDriver postedWorkRunsBeforeResume(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		EventLog &events,
		ThreadRecorder &threadsSeen)
	{
		(void)exceptionPtr;
		(void)completion;

		boost::asio::post(
			sscl::ComponentThread::getSelf()->getIoContext(),
			[&events]()
			{
				events.push(eventPostedBeforeYield);
			});

		co_await sscl::co::getYieldInvoker();
		threadsSeen.record(threadsSeen.afterYieldThread);
		events.push(eventAfterFirstYield);
		co_return;
	}

	static CallerDriver consecutiveYieldsAllowPostedWorkInGap(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		EventLog &events)
	{
		(void)exceptionPtr;
		(void)completion;

		boost::asio::post(
			sscl::ComponentThread::getSelf()->getIoContext(),
			[&events]()
			{
				events.push(eventPostedBeforeYield);
			});
		co_await sscl::co::getYieldInvoker();
		events.push(eventAfterFirstYield);

		boost::asio::post(
			sscl::ComponentThread::getSelf()->getIoContext(),
			[&events]()
			{
				events.push(eventPostedBetweenYields);
			});
		co_await sscl::co::getYieldInvoker();
		events.push(eventAfterSecondYield);
		co_return;
	}

	static CalleeViralInvoker<int> yieldThenReturnValue(int value)
	{
		co_await sscl::co::getYieldInvoker();
		co_return value;
	}

	static CallerDriver nestedViralCalleeYields(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		ThreadRecorder &threadsSeen)
	{
		(void)exceptionPtr;
		(void)completion;

		const int value = co_await yieldThenReturnValue(nestedCalleeReturnValue);
		threadsSeen.record(threadsSeen.awaitResumeThread);
		if (value != nestedCalleeReturnValue)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": nested callee returned "
				+ std::to_string(value)
				+ ", expected "
				+ std::to_string(nestedCalleeReturnValue));
		}

		co_return;
	}

	static CalleeViralInvoker<int> yieldThenThrow()
	{
		co_await sscl::co::getYieldInvoker();
		throw std::runtime_error(expectedYieldThrowMessage);
	}

	static CallerDriver yieldThenThrowObservedOnCaller(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		ThreadRecorder &threadsSeen)
	{
		(void)exceptionPtr;
		(void)completion;

		try
		{
			(void)co_await yieldThenThrow();
			throw std::runtime_error(
				std::string(__func__)
				+ ": expected callee to throw after yield");
		}
		catch (const std::runtime_error &runtimeError)
		{
			threadsSeen.record(threadsSeen.awaitResumeThread);
			if (std::string(runtimeError.what()) != expectedYieldThrowMessage)
			{
				throw std::runtime_error(
					std::string(__func__)
					+ ": unexpected exception message: "
					+ runtimeError.what());
			}
		}

		co_return;
	}

	static CallerDriver yieldThenTimerThenYield(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		ThreadRecorder &threadsSeen)
	{
		(void)exceptionPtr;
		(void)completion;

		co_await sscl::co::getYieldInvoker();
		const boost::system::error_code waitError =
			co_await sscl::tests::DeadlineTimerAwaiter{
				sscl::ComponentThread::getSelf()->getIoContext(),
				timerThenYieldDelayMs};
		sscl::tests::throwIfTimerWaitFailed(waitError);
		co_await sscl::co::getYieldInvoker();
		threadsSeen.record(threadsSeen.afterYieldThread);
		co_return;
	}
};

class YieldCoQutexProducers
{
public:
	static CalleeViralInvoker<void> holdLockYieldThenRelease(
		sscl::co::CoQutex &lock,
		QutexOccupancy &occupancy)
	{
		auto releaseHandle =
			co_await lock.getAcquireInvocationAndSuspensionPolicy();
		occupancy.holderHolds.store(true, std::memory_order_release);
		co_await sscl::co::getYieldInvoker();
		if (occupancy.waiterEntered.load(std::memory_order_acquire))
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": waiter entered CoQutex while holder was yielding");
		}

		occupancy.holderHolds.store(false, std::memory_order_release);
		releaseHandle.release();
		occupancy.holderReleased.store(true, std::memory_order_release);
		co_return;
	}

	static CalleeViralInvoker<void> acquireAfterContenderHolds(
		sscl::co::CoQutex &lock,
		QutexOccupancy &occupancy)
	{
		auto releaseHandle =
			co_await lock.getAcquireInvocationAndSuspensionPolicy();
		if (occupancy.holderHolds.load(std::memory_order_acquire))
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": waiter acquired CoQutex while holder still held it");
		}

		occupancy.waiterEntered.store(true, std::memory_order_release);
		releaseHandle.release();
		co_return;
	}

	static CalleeViralInvoker<void> yieldThenAcquire(
		sscl::co::CoQutex &lock,
		QutexOccupancy &occupancy)
	{
		co_await sscl::co::getYieldInvoker();
		auto releaseHandle =
			co_await lock.getAcquireInvocationAndSuspensionPolicy();
		if (!occupancy.holderReleased.load(std::memory_order_acquire))
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": waiter acquired CoQutex before holder released");
		}

		occupancy.waiterEntered.store(true, std::memory_order_release);
		releaseHandle.release();
		co_return;
	}

	static CalleeViralInvoker<void> acquireReleaseThenYield(
		sscl::co::CoQutex &lock,
		QutexOccupancy &occupancy)
	{
		auto releaseHandle =
			co_await lock.getAcquireInvocationAndSuspensionPolicy();
		occupancy.holderHolds.store(true, std::memory_order_release);
		occupancy.holderHolds.store(false, std::memory_order_release);
		releaseHandle.release();
		occupancy.holderReleased.store(true, std::memory_order_release);
		co_await sscl::co::getYieldInvoker();
		co_return;
	}

	static CallerDriver runHolderThenWaiter(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::co::CoQutex &lock,
		QutexOccupancy &occupancy)
	{
		(void)exceptionPtr;
		(void)completion;

		CalleeViralInvoker<void> holder =
			holdLockYieldThenRelease(lock, occupancy);
		CalleeViralInvoker<void> waiter =
			acquireAfterContenderHolds(lock, occupancy);
		sscl::co::Group group;
		group.add(holder);
		group.add(waiter);
		(void)co_await group.getAwaitAllSettlementsInvoker();
		co_return;
	}

	static CallerDriver runWaiterYieldThenHolder(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::co::CoQutex &lock,
		QutexOccupancy &occupancy)
	{
		(void)exceptionPtr;
		(void)completion;

		CalleeViralInvoker<void> waiter = yieldThenAcquire(lock, occupancy);
		CalleeViralInvoker<void> holder =
			holdLockYieldThenRelease(lock, occupancy);
		sscl::co::Group group;
		group.add(waiter);
		group.add(holder);
		(void)co_await group.getAwaitAllSettlementsInvoker();
		co_return;
	}

	static CallerDriver runReleaseThenWaiterAcquire(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::co::CoQutex &lock,
		QutexOccupancy &occupancy)
	{
		(void)exceptionPtr;
		(void)completion;

		CalleeViralInvoker<void> waiter = yieldThenAcquire(lock, occupancy);
		CalleeViralInvoker<void> holder =
			acquireReleaseThenYield(lock, occupancy);
		sscl::co::Group group;
		group.add(waiter);
		group.add(holder);
		(void)co_await group.getAwaitAllSettlementsInvoker();
		co_return;
	}
};

class YieldCvProducers
{
public:
	static CalleeViralInvoker<void> yieldThenWait(
		sscl::co::CoConditionVariable &cv)
	{
		co_await sscl::co::getYieldInvoker();
		co_await cv.getWaitForInvoker();
		co_return;
	}

	static CalleeViralInvoker<void> yieldThenSignal(
		sscl::co::CoConditionVariable &cv)
	{
		co_await sscl::co::getYieldInvoker();
		cv.signal();
		co_return;
	}

	static CallerDriver waitThenSignalAcrossYields(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::co::CoConditionVariable &cv)
	{
		(void)exceptionPtr;
		(void)completion;

		CalleeViralInvoker<void> waiter = yieldThenWait(cv);
		CalleeViralInvoker<void> signaler = yieldThenSignal(cv);
		sscl::co::Group group;
		group.add(waiter);
		group.add(signaler);
		(void)co_await group.getAwaitAllSettlementsInvoker();
		co_return;
	}

	static CallerDriver signalPostedBeforeYieldThenWait(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::co::CoConditionVariable &cv)
	{
		(void)exceptionPtr;
		(void)completion;

		boost::asio::post(
			sscl::ComponentThread::getSelf()->getIoContext(),
			[&cv]()
			{
				cv.signal();
			});
		co_await sscl::co::getYieldInvoker();
		co_await cv.getWaitForInvoker();
		co_return;
	}

	static CallerDriver yieldAfterAlreadySignaledThenWait(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::co::CoConditionVariable &cv)
	{
		(void)exceptionPtr;
		(void)completion;

		cv.signal();
		co_await sscl::co::getYieldInvoker();
		co_await cv.getWaitForInvoker();
		co_return;
	}
};

class YieldGroupProducers
{
public:
	static CalleeViralInvoker<int> yieldThenReturn(int value)
	{
		co_await sscl::co::getYieldInvoker();
		co_return value;
	}

	static CalleeViralInvoker<int> returnImmediately(int value)
	{
		co_return value;
	}

	static CalleeViralInvoker<int> waitThenReturnLabel(int delayMilliseconds)
	{
		const boost::system::error_code waitError =
			co_await sscl::tests::DeadlineTimerAwaiter{
				sscl::ComponentThread::getSelf()->getIoContext(),
				delayMilliseconds};
		sscl::tests::throwIfTimerWaitFailed(waitError);
		co_return delayMilliseconds;
	}

	static CallerDriver yieldingMemberSettles(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion)
	{
		(void)exceptionPtr;
		(void)completion;

		CalleeViralInvoker<int> member =
			yieldThenReturn(yieldMemberReturnValue);
		sscl::co::Group group;
		group.add(member);
		auto &descriptors = co_await group.getAwaitAllSettlementsInvoker();
		if (descriptors.size() != 1)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": expected 1 settlement, got "
				+ std::to_string(descriptors.size()));
		}

		sscl::tests::requireCompletedIntSettlement<CalleeViralInvoker<int>>(
			descriptors.front(),
			yieldMemberReturnValue);
		co_return;
	}

	static CallerDriver yieldBeforeAwaitingGroup(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion)
	{
		(void)exceptionPtr;
		(void)completion;

		CalleeViralInvoker<int> yielding =
			yieldThenReturn(yieldMemberReturnValue);
		CalleeViralInvoker<int> immediate =
			returnImmediately(immediateMemberReturnValue);
		sscl::co::Group group;
		group.add(yielding);
		group.add(immediate);
		co_await sscl::co::getYieldInvoker();
		auto &descriptors = co_await group.getAwaitAllSettlementsInvoker();
		if (descriptors.size() != 2)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": expected 2 settlements, got "
				+ std::to_string(descriptors.size()));
		}

		co_return;
	}

	static CallerDriver yieldMemberAndTimerMember(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion)
	{
		(void)exceptionPtr;
		(void)completion;

		CalleeViralInvoker<int> yielding =
			yieldThenReturn(yieldMemberReturnValue);
		CalleeViralInvoker<int> timed =
			waitThenReturnLabel(timerMemberDelayMs);
		sscl::co::Group group;
		group.add(yielding);
		group.add(timed);
		auto &descriptors = co_await group.getAwaitAllSettlementsInvoker();
		if (descriptors.size() != 2)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": expected 2 settlements, got "
				+ std::to_string(descriptors.size()));
		}

		bool sawYieldValue = std::any_of(
			descriptors.begin(),
			descriptors.end(),
			[](const sscl::co::Group::SettlementDescriptor &descriptor)
			{
				sscl::tests::requireCompletedSettlement(descriptor);
				return sscl::tests::CoroutineDriver::completedReturnValue(
					descriptor.invokerAs<CalleeViralInvoker<int>>())
					== yieldMemberReturnValue;
			});
		bool sawTimerValue = std::any_of(
			descriptors.begin(),
			descriptors.end(),
			[](const sscl::co::Group::SettlementDescriptor &descriptor)
			{
				sscl::tests::requireCompletedSettlement(descriptor);
				return sscl::tests::CoroutineDriver::completedReturnValue(
					descriptor.invokerAs<CalleeViralInvoker<int>>())
					== timerMemberDelayMs;
			});

		if (!sawYieldValue || !sawTimerValue)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": missing yield or timer member settlement");
		}

		co_return;
	}
};

class YieldPostingProducers
{
public:
	static CalleeViralInvoker<int> yieldThenReturnOnCallee(
		sscl::tests::CrossThreadTrace &trace)
	{
		co_await sscl::co::getYieldInvoker();
		trace.recordCalleeExecutionThread();
		trace.recordFinalSuspendThread();
		co_return expectedReturnValue;
	}

	static CalleeViralInvoker<int> yieldThenReturnOnExplicitTarget(
		sscl::co::ExplicitPostTarget postTarget,
		sscl::tests::CrossThreadTrace &trace)
	{
		(void)postTarget;
		co_await sscl::co::getYieldInvoker();
		trace.recordCalleeExecutionThread();
		co_return explicitTargetReturnValue;
	}

	static sscl::co::ViralNonPostingInvoker<int> yieldThenReturnNonPosting(
		int value,
		ThreadRecorder &threadsSeen)
	{
		co_await sscl::co::getYieldInvoker();
		threadsSeen.record(threadsSeen.calleeThread);
		co_return value;
	}

	static CallerDriver awaitCalleeThatYields(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::tests::CrossThreadTrace &trace)
	{
		(void)exceptionPtr;
		(void)completion;

		const int value = co_await yieldThenReturnOnCallee(trace);
		trace.recordAwaitResumeThread();
		if (value != expectedReturnValue)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": unexpected callee return value "
				+ std::to_string(value));
		}

		co_return;
	}

	static CallerDriver yieldThenAwaitCallee(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::tests::CrossThreadTrace &trace)
	{
		(void)exceptionPtr;
		(void)completion;

		co_await sscl::co::getYieldInvoker();
		const int value = co_await yieldThenReturnOnCallee(trace);
		trace.recordAwaitResumeThread();
		if (value != expectedReturnValue)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": unexpected callee return value "
				+ std::to_string(value));
		}

		co_return;
	}

	static CallerDriver awaitNonPostingThatYields(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		ThreadRecorder &threadsSeen)
	{
		(void)exceptionPtr;
		(void)completion;

		const int value = co_await yieldThenReturnNonPosting(
			nonPostingReturnValue,
			threadsSeen);
		threadsSeen.record(threadsSeen.awaitResumeThread);
		if (value != nonPostingReturnValue)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": unexpected non-posting return value "
				+ std::to_string(value));
		}

		co_return;
	}

	static CallerDriver awaitExplicitTargetThatYields(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		sscl::tests::CrossThreadTrace &trace)
	{
		(void)exceptionPtr;
		(void)completion;

		sscl::co::ExplicitPostTarget postTarget{
			sscl::tests::ThreadRegistry::ioContext(
				sscl::tests::PostingThreadRole::ALTERNATE)};
		const int value = co_await yieldThenReturnOnExplicitTarget(
			postTarget,
			trace);
		trace.recordAwaitResumeThread();
		if (value != explicitTargetReturnValue)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": unexpected explicit-target return value "
				+ std::to_string(value));
		}

		co_return;
	}
};

class YieldNurseryProducers
{
public:
	static sscl::co::NonViralNonPostingInvoker yieldThenCompleteCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion)
	{
		(void)exceptionPtr;
		(void)completion;
		co_await sscl::co::getYieldInvoker();
		co_return;
	}

	static sscl::co::NonViralNonPostingInvoker immediateCompleteCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion)
	{
		(void)exceptionPtr;
		(void)completion;
		co_return;
	}

	static sscl::co::NonViralNonPostingInvoker yieldThenThrowCReq(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion)
	{
		(void)exceptionPtr;
		(void)completion;
		co_await sscl::co::getYieldInvoker();
		throw std::runtime_error(expectedNurseryThrowMessage);
	}

	static CallerDriver drainYieldingMember(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion)
	{
		(void)exceptionPtr;
		(void)completion;

		sscl::co::NonViralTaskNursery nursery;
		nursery.openAdmission();
		nursery.launch(
			[](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
			{
				return yieldThenCompleteCReq(
					lease.getExceptionStorage(),
					lease.getCallerLambda());
			});
		nursery.closeAdmission();
		while (!nursery.allSettled())
			{ co_await sscl::co::getYieldInvoker(); }

		if (nursery.unsettledCount() != 0)
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": nursery still has unsettled slots after drain");
		}

		co_return;
	}

	static CallerDriver mixedImmediateAndYieldingMembers(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		std::atomic<std::size_t> &unsettledAfterLaunch)
	{
		(void)exceptionPtr;
		(void)completion;

		sscl::co::NonViralTaskNursery nursery;
		nursery.openAdmission();
		nursery.launch(
			[](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
			{
				return immediateCompleteCReq(
					lease.getExceptionStorage(),
					lease.getCallerLambda());
			});
		nursery.launch(
			[](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
			{
				return yieldThenCompleteCReq(
					lease.getExceptionStorage(),
					lease.getCallerLambda());
			});
		unsettledAfterLaunch.store(
			nursery.unsettledCount(),
			std::memory_order_release);
		nursery.closeAdmission();
		while (!nursery.allSettled())
			{ co_await sscl::co::getYieldInvoker(); }

		co_return;
	}

	static CallerDriver yieldingMemberThrowIsRecorded(
		std::exception_ptr &exceptionPtr,
		std::function<void()> completion,
		std::exception_ptr &captured)
	{
		(void)exceptionPtr;
		(void)completion;

		sscl::co::NonViralTaskNursery nursery;
		nursery.openAdmission();
		nursery.launch(
			[](sscl::co::NonViralTaskNursery::Slot::Lease &lease)
			{
				return yieldThenThrowCReq(
					lease.getExceptionStorage(),
					lease.getCallerLambda());
			},
			[&captured](std::exception_ptr &settledException)
			{
				captured = settledException;
			});
		nursery.closeAdmission();
		while (!nursery.allSettled())
			{ co_await sscl::co::getYieldInvoker(); }

		co_return;
	}
};

} // namespace

TEST_F(YieldInvokerTest, PostedWorkRunsBeforeResumeOnSameThread)
{
	EventLog events;
	ThreadRecorder threadsSeen;

	ASSERT_NO_THROW(
		runOnCaller(
			[&events, &threadsSeen](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoreProducers::postedWorkRunsBeforeResume(
					exceptionPtr,
					std::move(completion),
					events,
					threadsSeen);
			}));

	EXPECT_EQ(
		events.snapshot(),
		(std::vector<int>{eventPostedBeforeYield, eventAfterFirstYield}));
	EXPECT_EQ(threadsSeen.read(threadsSeen.afterYieldThread),
		threads.caller().osThreadId());
}

TEST_F(YieldInvokerTest, ConsecutiveYieldsLetPostedWorkRunInTheGap)
{
	EventLog events;

	ASSERT_NO_THROW(
		runOnCaller(
			[&events](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoreProducers::consecutiveYieldsAllowPostedWorkInGap(
					exceptionPtr,
					std::move(completion),
					events);
			}));

	EXPECT_EQ(
		events.snapshot(),
		(std::vector<int>{
			eventPostedBeforeYield,
			eventAfterFirstYield,
			eventPostedBetweenYields,
			eventAfterSecondYield}));
}

TEST_F(YieldInvokerTest, NestedViralCalleeYieldsThenReturns)
{
	ThreadRecorder threadsSeen;

	ASSERT_NO_THROW(
		runOnCaller(
			[&threadsSeen](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoreProducers::nestedViralCalleeYields(
					exceptionPtr,
					std::move(completion),
					threadsSeen);
			}));

	EXPECT_EQ(threadsSeen.read(threadsSeen.awaitResumeThread),
		threads.caller().osThreadId());
}

TEST_F(YieldInvokerTest, YieldThenThrowIsObservedOnCaller)
{
	ThreadRecorder threadsSeen;

	ASSERT_NO_THROW(
		runOnCaller(
			[&threadsSeen](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoreProducers::yieldThenThrowObservedOnCaller(
					exceptionPtr,
					std::move(completion),
					threadsSeen);
			}));

	EXPECT_EQ(threadsSeen.read(threadsSeen.awaitResumeThread),
		threads.caller().osThreadId());
}

TEST_F(YieldInvokerTest, YieldThenTimerThenYieldResumesOnCaller)
{
	ThreadRecorder threadsSeen;

	ASSERT_NO_THROW(
		runOnCaller(
			[&threadsSeen](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoreProducers::yieldThenTimerThenYield(
					exceptionPtr,
					std::move(completion),
					threadsSeen);
			}));

	EXPECT_EQ(threadsSeen.read(threadsSeen.afterYieldThread),
		threads.caller().osThreadId());
}

TEST_F(YieldInvokerTest, CoQutexHeldAcrossYieldBlocksWaiter)
{
	sscl::co::CoQutex lock("yield-coqutex-hold");
	QutexOccupancy occupancy;

	ASSERT_NO_THROW(
		runOnCaller(
			[&lock, &occupancy](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoQutexProducers::runHolderThenWaiter(
					exceptionPtr,
					std::move(completion),
					lock,
					occupancy);
			}));

	EXPECT_TRUE(occupancy.waiterEntered.load(std::memory_order_acquire));
	EXPECT_TRUE(occupancy.holderReleased.load(std::memory_order_acquire));
}

TEST_F(YieldInvokerTest, CoQutexYieldBeforeAcquireWaitsForHolder)
{
	sscl::co::CoQutex lock("yield-coqutex-before-acquire");
	QutexOccupancy occupancy;

	ASSERT_NO_THROW(
		runOnCaller(
			[&lock, &occupancy](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoQutexProducers::runWaiterYieldThenHolder(
					exceptionPtr,
					std::move(completion),
					lock,
					occupancy);
			}));

	EXPECT_TRUE(occupancy.waiterEntered.load(std::memory_order_acquire));
	EXPECT_TRUE(occupancy.holderReleased.load(std::memory_order_acquire));
}

TEST_F(YieldInvokerTest, CoQutexYieldAfterReleaseLetsWaiterProceed)
{
	sscl::co::CoQutex lock("yield-coqutex-after-release");
	QutexOccupancy occupancy;

	ASSERT_NO_THROW(
		runOnCaller(
			[&lock, &occupancy](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCoQutexProducers::runReleaseThenWaiterAcquire(
					exceptionPtr,
					std::move(completion),
					lock,
					occupancy);
			}));

	EXPECT_TRUE(occupancy.waiterEntered.load(std::memory_order_acquire));
	EXPECT_TRUE(occupancy.holderReleased.load(std::memory_order_acquire));
}

TEST_F(YieldInvokerTest, CoConditionVariableWaitAfterYieldIsSignaled)
{
	sscl::co::CoConditionVariable cv;

	ASSERT_NO_THROW(
		runOnCaller(
			[&cv](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCvProducers::waitThenSignalAcrossYields(
					exceptionPtr,
					std::move(completion),
					cv);
			}));
}

TEST_F(YieldInvokerTest, CoConditionVariableSignalWhileYieldPostedThenWait)
{
	sscl::co::CoConditionVariable cv;

	ASSERT_NO_THROW(
		runOnCaller(
			[&cv](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCvProducers::signalPostedBeforeYieldThenWait(
					exceptionPtr,
					std::move(completion),
					cv);
			}));
}

TEST_F(YieldInvokerTest, CoConditionVariableWaitAfterYieldSeesPriorSignal)
{
	sscl::co::CoConditionVariable cv;

	ASSERT_NO_THROW(
		runOnCaller(
			[&cv](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldCvProducers::yieldAfterAlreadySignaledThenWait(
					exceptionPtr,
					std::move(completion),
					cv);
			}));
}

TEST_F(YieldInvokerTest, GroupMemberYieldThenReturnSettles)
{
	ASSERT_NO_THROW(
		runOnCaller(
			[](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldGroupProducers::yieldingMemberSettles(
					exceptionPtr,
					std::move(completion));
			}));
}

TEST_F(YieldInvokerTest, YieldBeforeAwaitingGroupStillSettlesMembers)
{
	ASSERT_NO_THROW(
		runOnCaller(
			[](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldGroupProducers::yieldBeforeAwaitingGroup(
					exceptionPtr,
					std::move(completion));
			}));
}

TEST_F(YieldInvokerTest, GroupKeepsYieldingAndTimerMembers)
{
	ASSERT_NO_THROW(
		runOnCaller(
			[](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldGroupProducers::yieldMemberAndTimerMember(
					exceptionPtr,
					std::move(completion));
			}));
}

TEST_F(YieldInvokerTest, ViralCalleeYieldPostsBackToCaller)
{
	sscl::tests::CrossThreadTrace trace;

	ASSERT_NO_THROW(
		runOnCaller(
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				trace.recordConstructionThread();
				return YieldPostingProducers::awaitCalleeThatYields(
					exceptionPtr,
					std::move(completion),
					trace);
			}));

	EXPECT_EQ(trace.constructionThread(), threads.caller().osThreadId());
	EXPECT_EQ(trace.calleeExecutionThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.awaitResumeThread(), threads.caller().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), trace.awaitResumeThread());
}

TEST_F(YieldInvokerTest, CallerYieldThenViralAwaitKeepsPostBackThreads)
{
	sscl::tests::CrossThreadTrace trace;

	ASSERT_NO_THROW(
		runOnCaller(
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				trace.recordConstructionThread();
				return YieldPostingProducers::yieldThenAwaitCallee(
					exceptionPtr,
					std::move(completion),
					trace);
			}));

	EXPECT_EQ(trace.constructionThread(), threads.caller().osThreadId());
	EXPECT_EQ(trace.calleeExecutionThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.awaitResumeThread(), threads.caller().osThreadId());
}

TEST_F(YieldInvokerTest, ViralNonPostingYieldStaysOnCallerThread)
{
	ThreadRecorder threadsSeen;

	ASSERT_NO_THROW(
		runOnCaller(
			[&threadsSeen](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldPostingProducers::awaitNonPostingThatYields(
					exceptionPtr,
					std::move(completion),
					threadsSeen);
			}));

	EXPECT_EQ(threadsSeen.read(threadsSeen.calleeThread),
		threads.caller().osThreadId());
	EXPECT_EQ(threadsSeen.read(threadsSeen.awaitResumeThread),
		threads.caller().osThreadId());
}

TEST_F(YieldInvokerTest, ExplicitPostTargetHonoredWhenCalleeYields)
{
	sscl::tests::CrossThreadTrace trace;

	ASSERT_NO_THROW(
		runOnCaller(
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				trace.recordConstructionThread();
				return YieldPostingProducers::awaitExplicitTargetThatYields(
					exceptionPtr,
					std::move(completion),
					trace);
			}));

	EXPECT_EQ(trace.constructionThread(), threads.caller().osThreadId());
	EXPECT_EQ(trace.calleeExecutionThread(), threads.alternate().osThreadId());
	EXPECT_EQ(trace.awaitResumeThread(), threads.caller().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), threads.callee().osThreadId());
}

TEST_F(YieldInvokerTest, NurseryDrainsAfterMemberYields)
{
	ASSERT_NO_THROW(
		runOnCaller(
			[](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldNurseryProducers::drainYieldingMember(
					exceptionPtr,
					std::move(completion));
			}));
}

TEST_F(YieldInvokerTest, NurseryUnsettledCountTracksYieldingMember)
{
	std::atomic<std::size_t> unsettledAfterLaunch{0};

	ASSERT_NO_THROW(
		runOnCaller(
			[&unsettledAfterLaunch](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldNurseryProducers::mixedImmediateAndYieldingMembers(
					exceptionPtr,
					std::move(completion),
					unsettledAfterLaunch);
			}));

	EXPECT_EQ(unsettledAfterLaunch.load(std::memory_order_acquire), 1U);
}

TEST_F(YieldInvokerTest, NurseryRecordsExceptionAfterYieldThenThrow)
{
	std::exception_ptr captured;

	ASSERT_NO_THROW(
		runOnCaller(
			[&captured](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return YieldNurseryProducers::yieldingMemberThrowIsRecorded(
					exceptionPtr,
					std::move(completion),
					captured);
			}));

	ASSERT_TRUE(captured != nullptr);
	try
	{
		std::rethrow_exception(captured);
	}
	catch (const std::runtime_error &runtimeError)
	{
		EXPECT_EQ(std::string(runtimeError.what()), expectedNurseryThrowMessage);
	}
}
