#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <spinscale/co/coQutex.h>

#include <support/threadHarness.h>

namespace {

constexpr int leftValue = 1;
constexpr int rightValue = 2;
constexpr int expectedIntSum = 3;
constexpr int bodyArgument = 4;
constexpr const char *bodyStringArgument = "KEKW";
constexpr const char *leftString = "Hello";
constexpr const char *rightString = "World";
constexpr const char *expectedString = "Hello World";

using BodyNonViralInvoker =
	sscl::tests::RoleNonViralPostingInvoker<
		sscl::tests::PostingThreadRole::BODY>;

template <typename T>
using BodyViralInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::BODY,
		T>;

template <typename T>
using WorldViralInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::WORLD,
		T>;

template <typename T>
using LegViralInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::LEG,
		T>;

class ComponentContinuationTrace
{
public:
	void recordBodyThread()
	{
		std::lock_guard<std::mutex> guard(mutex);
		bodyThreadId = std::this_thread::get_id();
	}

	void recordWorldThread()
	{
		std::lock_guard<std::mutex> guard(mutex);
		worldThreadId = std::this_thread::get_id();
	}

	void recordLegThread()
	{
		std::lock_guard<std::mutex> guard(mutex);
		legThreadId = std::this_thread::get_id();
	}

	void recordCompletionThread()
	{
		std::lock_guard<std::mutex> guard(mutex);
		completionThreadId = std::this_thread::get_id();
	}

	void recordLegSum(int value)
	{
		std::lock_guard<std::mutex> guard(mutex);
		legSum = value;
	}

	void recordWorldString(std::string value)
	{
		std::lock_guard<std::mutex> guard(mutex);
		worldString = std::move(value);
	}

	void recordBodyString(std::string value)
	{
		std::lock_guard<std::mutex> guard(mutex);
		bodyString = std::move(value);
	}

	std::thread::id bodyThread() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return bodyThreadId;
	}

	std::thread::id worldThread() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return worldThreadId;
	}

	std::thread::id legThread() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return legThreadId;
	}

	std::thread::id completionThread() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return completionThreadId;
	}

	int recordedLegSum() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return legSum;
	}

	std::string recordedWorldString() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return worldString;
	}

	std::string recordedBodyString() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return bodyString;
	}

private:
	mutable std::mutex mutex;
	std::thread::id bodyThreadId;
	std::thread::id worldThreadId;
	std::thread::id legThreadId;
	std::thread::id completionThreadId;
	int legSum = 0;
	std::string worldString;
	std::string bodyString;
};

LegViralInvoker<int> print2Ints(
	int arg1,
	int arg2,
	ComponentContinuationTrace &trace)
{
	sscl::co::CoQutex print2IntsLock;
	trace.recordLegThread();
	auto releaseHandle =
		co_await print2IntsLock.getAcquireInvocationAndSuspensionPolicy();
	const int sum = arg1 + arg2;
	trace.recordLegSum(sum);
	releaseHandle.release();
	co_return sum;
}

WorldViralInvoker<std::string> print2Strings(
	std::string arg1,
	std::string arg2,
	ComponentContinuationTrace &trace)
{
	sscl::co::CoQutex print2StringsLock;
	trace.recordWorldThread();
	auto releaseHandle =
		co_await print2StringsLock.getAcquireInvocationAndSuspensionPolicy();
	const int returnedInt =
		co_await print2Ints(leftValue, rightValue, trace);
	releaseHandle.release();

	if (returnedInt != expectedIntSum) {
		throw std::runtime_error("LEG int return mismatch");
	}

	std::string returnedString = arg1 + " " + arg2;
	trace.recordWorldString(returnedString);
	co_return returnedString;
}

BodyNonViralInvoker initializeDemoCReq(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	int arg3,
	std::string arg4,
	ComponentContinuationTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;
	(void)arg3;
	(void)arg4;

	sscl::co::CoQutex initializeLock;
	trace.recordBodyThread();
	auto releaseHandle =
		co_await initializeLock.getAcquireInvocationAndSuspensionPolicy();
	std::string returnedString =
		co_await print2Strings(leftString, rightString, trace);
	releaseHandle.release();

	trace.recordBodyString(returnedString);
	co_return;
}

class ComponentContinuationTest
:	public ::testing::Test
{
protected:
	sscl::tests::PostingThreadSet threads;
};

} // namespace

TEST_F(ComponentContinuationTest, SyncMainStyleContinuationCrossesComponentThreads)
{
	ComponentContinuationTrace trace;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return initializeDemoCReq(
					exceptionPtr,
					[&trace, completion = std::move(completion)]() mutable
					{
						trace.recordCompletionThread();
						completion();
					},
					bodyArgument,
					bodyStringArgument,
					trace);
			}));

	EXPECT_EQ(trace.bodyThread(), threads.body().osThreadId());
	EXPECT_EQ(trace.worldThread(), threads.world().osThreadId());
	EXPECT_EQ(trace.legThread(), threads.leg().osThreadId());
	EXPECT_EQ(trace.completionThread(), threads.caller().osThreadId());

	EXPECT_NE(trace.bodyThread(), trace.worldThread());
	EXPECT_NE(trace.worldThread(), trace.legThread());
	EXPECT_NE(trace.legThread(), trace.completionThread());

	EXPECT_EQ(trace.recordedLegSum(), expectedIntSum);
	EXPECT_EQ(trace.recordedWorldString(), expectedString);
	EXPECT_EQ(trace.recordedBodyString(), expectedString);
}
