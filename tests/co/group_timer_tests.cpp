#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include <spinscale/co/group.h>
#include <spinscale/componentThread.h>

#include <support/groupAssertions.h>
#include <support/threadHarness.h>
#include <support/timerAwaiters.h>

namespace {

constexpr int timerDelayShortMs = 50;
constexpr int timerDelayMediumMs = 200;
constexpr int timerDelayLongMs = 500;
constexpr int awaitAllTimingSlackMs = 25;
constexpr int awaitAllLongCancelTimingMarginMs = 50;

using CallerDriver =
	sscl::tests::RoleNonViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLER>;

using CalleeIntInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLEE,
		int>;

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

class GroupTimerThreadTrace
{
public:
	void recordTimerCompletionThread(int timerLabelMilliseconds)
	{
		std::lock_guard<std::mutex> guard(mutex);
		timerCompletionThreads[timerLabelMilliseconds] =
			std::this_thread::get_id();
	}

	void recordAwaitFirstResumeThread()
	{
		std::lock_guard<std::mutex> guard(mutex);
		awaitFirstResumeThread = std::this_thread::get_id();
	}

	void recordAwaitAllResumeThread()
	{
		std::lock_guard<std::mutex> guard(mutex);
		awaitAllResumeThread = std::this_thread::get_id();
	}

	std::thread::id timerCompletionThread(int timerLabelMilliseconds) const
	{
		std::lock_guard<std::mutex> guard(mutex);
		const auto iterator =
			timerCompletionThreads.find(timerLabelMilliseconds);

		if (iterator == timerCompletionThreads.end()) {
			throw std::runtime_error("Missing timer completion thread trace");
		}

		return iterator->second;
	}

	std::thread::id awaitFirstThread() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return awaitFirstResumeThread;
	}

	std::thread::id awaitAllThread() const
	{
		std::lock_guard<std::mutex> guard(mutex);
		return awaitAllResumeThread;
	}

private:
	mutable std::mutex mutex;
	std::map<int, std::thread::id> timerCompletionThreads;
	std::thread::id awaitFirstResumeThread;
	std::thread::id awaitAllResumeThread;
};

CalleeIntInvoker waitDeadlineTimer(
	int timerLabelMilliseconds,
	GroupTimerThreadTrace &trace)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			timerLabelMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	trace.recordTimerCompletionThread(timerLabelMilliseconds);
	co_return timerLabelMilliseconds;
}

CalleeIntInvoker waitCancelableDeadlineTimer(
	int timerLabelMilliseconds,
	sscl::tests::CancelableDeadlineTimerRegistry &registry,
	GroupTimerThreadTrace &trace)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::RegisteredDeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			timerLabelMilliseconds,
			timerLabelMilliseconds,
			registry};

	if (sscl::tests::timerWasCanceled(waitError)) {
		trace.recordTimerCompletionThread(timerLabelMilliseconds);
		co_return timerLabelMilliseconds;
	}

	sscl::tests::throwIfTimerWaitFailed(waitError);
	trace.recordTimerCompletionThread(timerLabelMilliseconds);
	co_return timerLabelMilliseconds;
}

void throwIfElapsedTooLong(
	const Ms &elapsed,
	const Ms &limit,
	const char *message)
{
	if (elapsed > limit) {
		throw std::runtime_error(
			std::string(message) + ": " + std::to_string(elapsed.count()));
	}
}

void throwIfElapsedTooShort(
	const Ms &elapsed,
	const Ms &limit,
	const char *message)
{
	if (elapsed < limit) {
		throw std::runtime_error(
			std::string(message) + ": " + std::to_string(elapsed.count()));
	}
}

CallerDriver runGroupTimerRace(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	GroupTimerThreadTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker invokerShort =
		waitDeadlineTimer(timerDelayShortMs, trace);
	CalleeIntInvoker invokerMedium =
		waitDeadlineTimer(timerDelayMediumMs, trace);
	CalleeIntInvoker invokerLong =
		waitDeadlineTimer(timerDelayLongMs, trace);

	group.add(invokerShort);
	group.add(invokerMedium);
	group.add(invokerLong);

	const auto testStart = Clock::now();

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstSettlement, allSettlementsAfterFirst] = co_await awaitFirst;
	trace.recordAwaitFirstResumeThread();

	const auto firstElapsedMs =
		std::chrono::duration_cast<Ms>(Clock::now() - testStart);
	throwIfElapsedTooLong(
		firstElapsedMs,
		Ms(timerDelayMediumMs - awaitAllTimingSlackMs),
		"await-first took too long");

	if (&firstSettlement.invokerAs<CalleeIntInvoker>() != &invokerShort) {
		throw std::runtime_error("first settlement was not shortest timer");
	}

	if (group.allInvokersSettled()) {
		throw std::runtime_error("await-first returned after all settled");
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allSettlements = co_await awaitAll;
	trace.recordAwaitAllResumeThread();

	const auto allElapsedMs =
		std::chrono::duration_cast<Ms>(Clock::now() - testStart);
	throwIfElapsedTooShort(
		allElapsedMs,
		Ms(timerDelayLongMs - awaitAllLongCancelTimingMarginMs),
		"await-all finished too soon");

	if (allSettlements.size() != 3) {
		throw std::runtime_error("expected three settlements");
	}

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstSettlement,
		timerDelayShortMs);
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allSettlementsAfterFirst[0],
		timerDelayShortMs);
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allSettlementsAfterFirst[1],
		timerDelayMediumMs);
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allSettlementsAfterFirst[2],
		timerDelayLongMs);

	co_return;
}

CallerDriver runGroupTimerCancelLongAfterAwaitFirst(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::tests::CancelableDeadlineTimerRegistry &registry,
	GroupTimerThreadTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker invokerShort =
		waitCancelableDeadlineTimer(timerDelayShortMs, registry, trace);
	CalleeIntInvoker invokerMedium =
		waitCancelableDeadlineTimer(timerDelayMediumMs, registry, trace);
	CalleeIntInvoker invokerLong =
		waitCancelableDeadlineTimer(timerDelayLongMs, registry, trace);

	group.add(invokerShort);
	group.add(invokerMedium);
	group.add(invokerLong);

	const auto testStart = Clock::now();

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstSettlement, allSettlementsAfterFirst] = co_await awaitFirst;
	trace.recordAwaitFirstResumeThread();

	if (&firstSettlement.invokerAs<CalleeIntInvoker>() != &invokerShort) {
		throw std::runtime_error("cancel test first settlement mismatch");
	}

	if (group.allInvokersSettled()) {
		throw std::runtime_error("cancel test all settled after await-first");
	}

	registry.cancel(timerDelayLongMs);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allSettlements = co_await awaitAll;
	trace.recordAwaitAllResumeThread();

	const auto allElapsedMs =
		std::chrono::duration_cast<Ms>(Clock::now() - testStart);

	if (allElapsedMs >= Ms(timerDelayLongMs - awaitAllLongCancelTimingMarginMs)) {
		throw std::runtime_error("await-all waited for canceled long timer");
	}

	throwIfElapsedTooShort(
		allElapsedMs,
		Ms(timerDelayMediumMs - awaitAllTimingSlackMs),
		"await-all finished before medium timer");

	if (allSettlements.size() != 3) {
		throw std::runtime_error("cancel test expected three settlements");
	}

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allSettlements[0],
		timerDelayShortMs);
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allSettlements[1],
		timerDelayMediumMs);
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allSettlements[2],
		timerDelayLongMs);

	if (&allSettlements[2].invokerAs<CalleeIntInvoker>() != &invokerLong) {
		throw std::runtime_error("cancel test long invoker mismatch");
	}

	(void)allSettlementsAfterFirst;
	co_return;
}

class GroupTimerTest
:	public ::testing::Test
{
protected:
	void assertTimerTraceCrossedThreads(
		const GroupTimerThreadTrace &trace)
	{
		EXPECT_EQ(
			trace.timerCompletionThread(timerDelayShortMs),
			threads.callee().osThreadId());
		EXPECT_EQ(
			trace.timerCompletionThread(timerDelayMediumMs),
			threads.callee().osThreadId());
		EXPECT_EQ(
			trace.timerCompletionThread(timerDelayLongMs),
			threads.callee().osThreadId());
		EXPECT_EQ(trace.awaitFirstThread(), threads.caller().osThreadId());
		EXPECT_EQ(trace.awaitAllThread(), threads.caller().osThreadId());
		EXPECT_NE(
			trace.timerCompletionThread(timerDelayShortMs),
			trace.awaitFirstThread());
	}

	sscl::tests::PostingThreadSet threads;
};

} // namespace

TEST_F(GroupTimerTest, AwaitFirstReturnsShortestTimerAndAwaitAllWaitsForLongest)
{
	GroupTimerThreadTrace trace;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return runGroupTimerRace(
					exceptionPtr,
					std::move(completion),
					trace);
			}));

	assertTimerTraceCrossedThreads(trace);
}

TEST_F(GroupTimerTest, CancelLongTimerAfterAwaitFirst)
{
	sscl::tests::CancelableDeadlineTimerRegistry registry;
	GroupTimerThreadTrace trace;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&registry, &trace](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return runGroupTimerCancelLongAfterAwaitFirst(
					exceptionPtr,
					std::move(completion),
					registry,
					trace);
			}));

	assertTimerTraceCrossedThreads(trace);
}
