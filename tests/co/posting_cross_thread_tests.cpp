#include <exception>
#include <functional>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <spinscale/co/postTarget.h>
#include <spinscale/componentThread.h>

#include <support/threadHarness.h>
#include <support/timerAwaiters.h>

namespace {

constexpr int expectedReturnValue = 42;
constexpr int explicitTargetReturnValue = 77;
constexpr const char *expectedThrowMessage =
	"posting cross-thread intentional failure";

using CallerNonViralInvoker =
	sscl::tests::RoleNonViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLER>;
using CalleeNonViralInvoker =
	sscl::tests::RoleNonViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLEE>;

template <typename T>
using CalleeViralInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLEE,
		T>;

CalleeViralInvoker<int> returnFromCalleeThread(
	sscl::tests::CrossThreadTrace &trace)
{
	trace.recordCalleeExecutionThread();
	trace.recordFinalSuspendThread();
	co_return expectedReturnValue;
}

CalleeViralInvoker<int> returnFromExplicitTargetThread(
	sscl::co::ExplicitPostTarget postTarget,
	sscl::tests::CrossThreadTrace &trace)
{
	(void)postTarget;
	trace.recordCalleeExecutionThread();
	trace.recordFinalSuspendThread();
	co_return explicitTargetReturnValue;
}

CalleeViralInvoker<int> throwFromCalleeThread(
	sscl::tests::CrossThreadTrace &trace)
{
	constexpr int throwDelayMs = 1;

	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			throwDelayMs};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	trace.recordCalleeExecutionThread();
	trace.recordFinalSuspendThread();
	throw std::runtime_error(expectedThrowMessage);
}

CallerNonViralInvoker awaitCalleeDriver(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::tests::CrossThreadTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;

	const int value = co_await returnFromCalleeThread(trace);
	trace.recordAwaitResumeThread();

	if (value != expectedReturnValue) {
		throw std::runtime_error("Unexpected callee return value");
	}

	co_return;
}

CallerNonViralInvoker awaitExplicitTargetDriver(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::tests::CrossThreadTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::ExplicitPostTarget postTarget{
		sscl::tests::ThreadRegistry::ioContext(
			sscl::tests::PostingThreadRole::ALTERNATE)};
	const int value = co_await returnFromExplicitTargetThread(
		postTarget,
		trace);
	trace.recordAwaitResumeThread();

	if (value != explicitTargetReturnValue) {
		throw std::runtime_error("Unexpected explicit-target return value");
	}

	co_return;
}

CallerNonViralInvoker awaitThrowingCalleeDriver(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::tests::CrossThreadTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;

	try {
		(void)co_await throwFromCalleeThread(trace);
		throw std::runtime_error("Expected callee exception");
	}
	catch (const std::runtime_error &runtimeError) {
		trace.recordAwaitResumeThread();
		if (std::string(runtimeError.what()) != expectedThrowMessage) {
			throw std::runtime_error("Unexpected callee exception message");
		}
	}

	co_return;
}

CalleeNonViralInvoker nonViralCalleeCompletesToCaller(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::tests::CrossThreadTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;
	trace.recordCalleeExecutionThread();
	trace.recordFinalSuspendThread();
	co_return;
}

class PostingCrossThreadTest
:	public ::testing::Test
{
protected:
	sscl::tests::PostingThreadSet threads;
};

} // namespace

TEST_F(PostingCrossThreadTest, ViralAwaitPostsCalleeAndResumesCaller)
{
	sscl::tests::CrossThreadTrace trace;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				trace.recordConstructionThread();
				return awaitCalleeDriver(
					exceptionPtr,
					std::move(completion),
					trace);
			}));

	EXPECT_EQ(trace.constructionThread(), threads.caller().osThreadId());
	EXPECT_EQ(trace.calleeExecutionThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.finalSuspendThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.awaitResumeThread(), threads.caller().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), trace.awaitResumeThread());
}

TEST_F(PostingCrossThreadTest, NonViralCompletionPostsBackToCaller)
{
	sscl::tests::CrossThreadTrace trace;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				trace.recordConstructionThread();
				return nonViralCalleeCompletesToCaller(
					exceptionPtr,
					[&trace, completion = std::move(completion)]() mutable
					{
						trace.recordCompletionCallbackThread();
						completion();
					},
					trace);
			}));

	EXPECT_EQ(trace.constructionThread(), threads.caller().osThreadId());
	EXPECT_EQ(trace.calleeExecutionThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.finalSuspendThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.completionCallbackThread(), threads.caller().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), trace.completionCallbackThread());
}

TEST_F(PostingCrossThreadTest, ExplicitPostTargetRoutesCalleeExecution)
{
	sscl::tests::CrossThreadTrace trace;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				trace.recordConstructionThread();
				return awaitExplicitTargetDriver(
					exceptionPtr,
					std::move(completion),
					trace);
			}));

	EXPECT_EQ(trace.constructionThread(), threads.caller().osThreadId());
	EXPECT_EQ(trace.calleeExecutionThread(), threads.alternate().osThreadId());
	EXPECT_EQ(trace.awaitResumeThread(), threads.caller().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), threads.callee().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), trace.awaitResumeThread());
}

TEST_F(PostingCrossThreadTest, CalleeExceptionIsObservedOnCallerThread)
{
	sscl::tests::CrossThreadTrace trace;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				trace.recordConstructionThread();
				return awaitThrowingCalleeDriver(
					exceptionPtr,
					std::move(completion),
					trace);
			}));

	EXPECT_EQ(trace.constructionThread(), threads.caller().osThreadId());
	EXPECT_EQ(trace.calleeExecutionThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.awaitResumeThread(), threads.caller().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), trace.awaitResumeThread());
}
