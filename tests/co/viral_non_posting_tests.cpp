#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>

#include <spinscale/co/invokers.h>
#include <spinscale/co/group.h>
#include <spinscale/componentThread.h>

#include <support/coroutineDriver.h>
#include <support/groupAssertions.h>
#include <support/threadHarness.h>
#include <support/timerAwaiters.h>

namespace {

constexpr int delayShortMs = 50;
constexpr int expectedNonStdThrowValue = 42;
constexpr const char *expectedThrowMessage =
	"viral_non_posting_test intentional failure";

template <typename T>
using TestInvoker = sscl::co::ViralNonPostingInvoker<T>;

using TestDriver = TestInvoker<int>;
using TestVoidDriver = TestInvoker<void>;
using CallerPostingDriver =
	sscl::tests::RoleNonViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLER>;

struct ThreadIdPair
{
	std::thread::id callerIdAtCoAwait;
	std::thread::id calleeId;
};

struct MoveCountedInt
{
	std::shared_ptr<std::size_t> moveCount;
	int value = 0;

	MoveCountedInt() = default;

	MoveCountedInt(
		std::shared_ptr<std::size_t> moveCountIn,
		int valueIn)
	: moveCount(std::move(moveCountIn)),
	value(valueIn)
	{}

	MoveCountedInt(const MoveCountedInt &) = delete;
	MoveCountedInt &operator=(const MoveCountedInt &) = delete;

	MoveCountedInt(MoveCountedInt &&other) noexcept
	: moveCount(std::exchange(other.moveCount, {})),
	value(other.value)
	{
		if (moveCount) {
			++(*moveCount);
		}
	}

	MoveCountedInt &operator=(MoveCountedInt &&other) noexcept
	{
		moveCount = std::exchange(other.moveCount, {});
		value = other.value;
		return *this;
	}
};

template <typename T>
struct CountingAwaiter
{
	TestInvoker<T> &invoker;
	std::size_t &awaitResumeCallCount;

	bool await_ready() const noexcept
		{ return invoker.await_ready(); }

	template <typename CallerPromise>
	bool await_suspend(
		std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
		{ return invoker.await_suspend(callerSchedHandle); }

	auto await_resume()
	{
		++awaitResumeCallCount;
		return invoker.await_resume();
	}
};

class ViralNonPostingTest
:	public ::testing::Test
{
protected:
	void TearDown() override
	{
		ioContext.restart();
	}

	int runDriver(TestDriver &driver)
	{
		return sscl::tests::CoroutineDriver::pumpUntilIdleAndReturnValue(
			ioContext,
			driver);
	}

	int finishDriver(TestDriver &driver)
	{
		return sscl::tests::CoroutineDriver::completedReturnValue(driver);
	}

	boost::asio::io_context ioContext;
};

TestInvoker<int> returnLabelImmediately(int label)
{
	co_return label;
}

TestInvoker<int> waitAndReturnLabel(
	boost::asio::io_context &ioContext,
	int delayMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			ioContext,
			delayMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	co_return delayMilliseconds;
}

TestVoidDriver voidReturnImmediately()
{
	co_return;
}

TestVoidDriver voidMemberAfterDelay(
	boost::asio::io_context &ioContext,
	int delayMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			ioContext,
			delayMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	co_return;
}

TestInvoker<int> throwRuntimeErrorImmediately()
{
	throw std::runtime_error(expectedThrowMessage);
}

TestInvoker<int> throwIntImmediately()
{
	throw expectedNonStdThrowValue;
}

TestInvoker<ThreadIdPair> recordThreadIdsAtReturn()
{
	ThreadIdPair pair;
	pair.calleeId = std::this_thread::get_id();
	co_return pair;
}

TestInvoker<ThreadIdPair> recordThreadIdsAfterDelay(
	boost::asio::io_context &ioContext,
	int delayMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			ioContext,
			delayMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);

	ThreadIdPair pair;
	pair.calleeId = std::this_thread::get_id();
	co_return pair;
}

TestInvoker<MoveCountedInt> returnMoveCountedInt(
	std::shared_ptr<std::size_t> moveCount,
	int value)
{
	co_return MoveCountedInt{std::move(moveCount), value};
}

TestInvoker<int> innerDelayedCoAwait(
	boost::asio::io_context &ioContext,
	int delayMilliseconds)
{
	const int label = co_await waitAndReturnLabel(
		ioContext,
		delayMilliseconds);
	co_return label;
}

TestInvoker<int> nestedNonPostingSum(int left, int right)
{
	const int leftSum = co_await returnLabelImmediately(left);
	const int rightSum = co_await returnLabelImmediately(right);
	co_return leftSum + rightSum;
}

TestInvoker<int> outerCoAwaitingDelayedInner(
	boost::asio::io_context &ioContext,
	int delayMilliseconds)
{
	const int innerLabel = co_await innerDelayedCoAwait(
		ioContext,
		delayMilliseconds);
	co_return innerLabel + 1;
}

TestDriver testImmediateReturnFastPath()
{
	const int value = co_await returnLabelImmediately(42);
	if (value != 42) {
		throw std::runtime_error("immediateReturnFastPath value mismatch");
	}
	co_return 0;
}

TestDriver testAllCompleteBeforeCoAwait()
{
	TestInvoker<int> invokerTen = returnLabelImmediately(10);
	TestInvoker<int> invokerTwenty = returnLabelImmediately(20);
	TestInvoker<int> invokerThirty = returnLabelImmediately(30);

	const int valueTen = co_await invokerTen;
	const int valueTwenty = co_await invokerTwenty;
	const int valueThirty = co_await invokerThirty;

	if (valueTen != 10 || valueTwenty != 20 || valueThirty != 30) {
		throw std::runtime_error("allCompleteBeforeCoAwait label mismatch");
	}

	co_return 0;
}

TestDriver testCallerSuspendsThenResumes(boost::asio::io_context &ioContext)
{
	const int value = co_await waitAndReturnLabel(ioContext, delayShortMs);
	if (value != delayShortMs) {
		throw std::runtime_error("callerSuspendsThenResumes label mismatch");
	}
	co_return 0;
}

TestDriver testMixedImmediateAndDelayedInSequence(
	boost::asio::io_context &ioContext)
{
	const int immediate = co_await returnLabelImmediately(7);
	const int delayed = co_await waitAndReturnLabel(ioContext, delayShortMs);

	if (immediate != 7 || delayed != delayShortMs) {
		throw std::runtime_error("mixedImmediateAndDelayed label mismatch");
	}

	co_return 0;
}

TestDriver testAwaitResumeCalledOnceFastPath()
{
	std::size_t awaitResumeCallCount = 0;
	TestInvoker<int> invoker = returnLabelImmediately(42);
	const int value = co_await CountingAwaiter<int>{
		invoker,
		awaitResumeCallCount};

	if (value != 42 || awaitResumeCallCount != 1) {
		throw std::runtime_error("fast path await_resume count mismatch");
	}

	co_return 0;
}

TestDriver testAwaitResumeCalledOnceSlowPath(
	boost::asio::io_context &ioContext)
{
	std::size_t awaitResumeCallCount = 0;
	TestInvoker<int> invoker = waitAndReturnLabel(ioContext, delayShortMs);
	const int value = co_await CountingAwaiter<int>{
		invoker,
		awaitResumeCallCount};

	if (value != delayShortMs || awaitResumeCallCount != 1) {
		throw std::runtime_error("slow path await_resume count mismatch");
	}

	co_return 0;
}

TestDriver testAwaitResumeCalledOnceNested(
	boost::asio::io_context &ioContext)
{
	std::size_t awaitResumeCallCount = 0;
	TestInvoker<int> inner = innerDelayedCoAwait(ioContext, delayShortMs);
	const int value = co_await CountingAwaiter<int>{
		inner,
		awaitResumeCallCount};

	if (value != delayShortMs || awaitResumeCallCount != 1) {
		throw std::runtime_error("nested await_resume count mismatch");
	}

	co_return 0;
}

TestDriver testMoveCountedReturnNotDoubleMoved()
{
	auto moveCount = std::make_shared<std::size_t>(0);
	TestInvoker<MoveCountedInt> invoker =
		returnMoveCountedInt(moveCount, 99);
	MoveCountedInt result = co_await invoker;

	if (result.value != 99) {
		throw std::runtime_error("move counted value mismatch");
	}
	if (*moveCount > 2 || *moveCount < 1) {
		throw std::runtime_error("move counted return move-count mismatch");
	}

	co_return 0;
}

TestDriver testVoidReturnCompletes()
{
	co_await voidReturnImmediately();
	co_return 0;
}

TestDriver testReturnValuesReadableBeforeDestroy()
{
	TestInvoker<int> invoker = returnLabelImmediately(55);
	(void)co_await invoker;

	if (invoker.completedReturnValues().myReturnValue != 55) {
		throw std::runtime_error("completed return value not readable");
	}

	co_return 0;
}

TestDriver testExceptionRethrowsOnCoAwait()
{
	try {
		(void)co_await throwRuntimeErrorImmediately();
		throw std::runtime_error("expected runtime_error");
	}
	catch (const std::runtime_error &runtimeError) {
		if (std::string(runtimeError.what()) != expectedThrowMessage) {
			throw std::runtime_error("unexpected runtime_error message");
		}
	}

	co_return 0;
}

TestDriver testNonStdExceptionRethrows()
{
	try {
		(void)co_await throwIntImmediately();
		throw std::runtime_error("expected int exception");
	}
	catch (int caughtValue) {
		if (caughtValue != expectedNonStdThrowValue) {
			throw std::runtime_error("unexpected int exception value");
		}
	}

	co_return 0;
}

TestDriver testCalleeRunsOnCallerThread()
{
	const std::thread::id callerThreadId = std::this_thread::get_id();
	const ThreadIdPair pair = co_await recordThreadIdsAtReturn();

	if (pair.calleeId != callerThreadId) {
		throw std::runtime_error("callee thread mismatch");
	}

	co_return 0;
}

TestDriver testDelayedCalleeStillOnCallerThread(
	boost::asio::io_context &ioContext)
{
	const std::thread::id callerThreadId = std::this_thread::get_id();
	const ThreadIdPair pair =
		co_await recordThreadIdsAfterDelay(ioContext, delayShortMs);

	if (pair.calleeId != callerThreadId) {
		throw std::runtime_error("delayed callee thread mismatch");
	}

	co_return 0;
}

TestDriver testNestedNonPostingCoAwait()
{
	const int sum = co_await nestedNonPostingSum(10, 32);
	if (sum != 42) {
		throw std::runtime_error("nested sum mismatch");
	}
	co_return 0;
}

TestDriver testNestedInnerSuspension(boost::asio::io_context &ioContext)
{
	const int value = co_await outerCoAwaitingDelayedInner(
		ioContext,
		delayShortMs);
	if (value != delayShortMs + 1) {
		throw std::runtime_error("nested inner suspension value mismatch");
	}
	co_return 0;
}

CallerPostingDriver nonPostingVoidMemberInGroupDriver(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	TestVoidDriver voidInvoker = voidMemberAfterDelay(
		sscl::ComponentThread::getSelf()->getIoContext(),
		delayShortMs);
	group.add(voidInvoker);

	auto &allDescriptors = co_await group.getAwaitAllSettlementsInvoker();

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("voidMemberInGroup count mismatch");
	}

	sscl::tests::requireCompletedSettlement(allDescriptors[0]);

	co_return;
}

CallerPostingDriver nonPostingGroupMixedImmediateAndDelayedDriver(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	TestInvoker<int> immediateInvoker = returnLabelImmediately(11);
	TestInvoker<int> delayedInvoker = waitAndReturnLabel(
		sscl::ComponentThread::getSelf()->getIoContext(),
		delayShortMs);

	group.add(immediateInvoker);
	group.add(delayedInvoker);

	auto &allDescriptors = co_await group.getAwaitAllSettlementsInvoker();

	if (allDescriptors.size() != 2) {
		throw std::runtime_error("groupMixedImmediateAndDelayed count mismatch");
	}

	bool sawImmediate = false;
	bool sawDelayed = false;

	for (auto &descriptor : allDescriptors) {
		sscl::tests::requireCompletedSettlement(descriptor);
		const int label = sscl::tests::completedIntValue(
			descriptor.invokerAs<TestInvoker<int>>());
		if (label == 11) {
			sawImmediate = true;
		}
		else if (label == delayShortMs) {
			sawDelayed = true;
		}
		else {
			throw std::runtime_error(
				"groupMixedImmediateAndDelayed unexpected label");
		}
	}

	if (!sawImmediate || !sawDelayed) {
		throw std::runtime_error(
			"groupMixedImmediateAndDelayed missing expected label");
	}

	co_return;
}

} // namespace

TEST_F(ViralNonPostingTest, ImmediateReturnFastPath)
{
	TestDriver driver = testImmediateReturnFastPath();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, AllCompleteBeforeCoAwait)
{
	TestDriver driver = testAllCompleteBeforeCoAwait();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, CallerSuspendsThenResumes)
{
	TestDriver driver = testCallerSuspendsThenResumes(ioContext);
	EXPECT_NO_THROW({ EXPECT_EQ(runDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, MixedImmediateAndDelayedInSequence)
{
	TestDriver driver = testMixedImmediateAndDelayedInSequence(ioContext);
	EXPECT_NO_THROW({ EXPECT_EQ(runDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, AwaitResumeCalledOnceFastPath)
{
	TestDriver driver = testAwaitResumeCalledOnceFastPath();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, AwaitResumeCalledOnceSlowPath)
{
	TestDriver driver = testAwaitResumeCalledOnceSlowPath(ioContext);
	EXPECT_NO_THROW({ EXPECT_EQ(runDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, AwaitResumeCalledOnceNested)
{
	TestDriver driver = testAwaitResumeCalledOnceNested(ioContext);
	EXPECT_NO_THROW({ EXPECT_EQ(runDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, MoveCountedReturnNotDoubleMoved)
{
	TestDriver driver = testMoveCountedReturnNotDoubleMoved();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, VoidReturnCompletes)
{
	TestDriver driver = testVoidReturnCompletes();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, ReturnValuesReadableBeforeDestroy)
{
	TestDriver driver = testReturnValuesReadableBeforeDestroy();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, ExceptionRethrowsOnCoAwait)
{
	TestDriver driver = testExceptionRethrowsOnCoAwait();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, NonStdExceptionRethrows)
{
	TestDriver driver = testNonStdExceptionRethrows();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, CalleeRunsOnCallerThread)
{
	TestDriver driver = testCalleeRunsOnCallerThread();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, DelayedCalleeStillOnCallerThread)
{
	TestDriver driver = testDelayedCalleeStillOnCallerThread(ioContext);
	EXPECT_NO_THROW({ EXPECT_EQ(runDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, NestedNonPostingCoAwait)
{
	TestDriver driver = testNestedNonPostingCoAwait();
	EXPECT_NO_THROW({ EXPECT_EQ(finishDriver(driver), 0); });
}

TEST_F(ViralNonPostingTest, NestedInnerSuspension)
{
	TestDriver driver = testNestedInnerSuspension(ioContext);
	EXPECT_NO_THROW({ EXPECT_EQ(runDriver(driver), 0); });
}

TEST(ViralNonPostingGroupIntegrationTest, VoidMemberInGroup)
{
	sscl::tests::PostingThreadSet threads;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return nonPostingVoidMemberInGroupDriver(
					exceptionPtr,
					std::move(completion));
			}));
}

TEST(ViralNonPostingGroupIntegrationTest, MixedImmediateAndDelayedInGroup)
{
	sscl::tests::PostingThreadSet threads;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return nonPostingGroupMixedImmediateAndDelayedDriver(
					exceptionPtr,
					std::move(completion));
			}));
}
