#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>

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

CalleeIntInvoker waitDeadlineTimer(int timerLabelMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			timerLabelMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	co_return timerLabelMilliseconds;
}

CalleeIntInvoker waitCancelableDeadlineTimer(
	int timerLabelMilliseconds,
	sscl::tests::CancelableDeadlineTimerRegistry &registry)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::RegisteredDeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			timerLabelMilliseconds,
			timerLabelMilliseconds,
			registry};

	if (sscl::tests::timerWasCanceled(waitError)) {
		co_return timerLabelMilliseconds;
	}

	sscl::tests::throwIfTimerWaitFailed(waitError);
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
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker invokerShort = waitDeadlineTimer(timerDelayShortMs);
	CalleeIntInvoker invokerMedium = waitDeadlineTimer(timerDelayMediumMs);
	CalleeIntInvoker invokerLong = waitDeadlineTimer(timerDelayLongMs);

	group.add(invokerShort);
	group.add(invokerMedium);
	group.add(invokerLong);

	const auto testStart = Clock::now();

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstSettlement, allSettlementsAfterFirst] = co_await awaitFirst;

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

	const auto allElapsedMs =
		std::chrono::duration_cast<Ms>(Clock::now() - testStart);
	throwIfElapsedTooShort(
		allElapsedMs,
		Ms(timerDelayLongMs - awaitAllLongCancelTimingMarginMs),
		"await-all finished too soon");

	if (allSettlements.size() != 3) {
		throw std::runtime_error("expected three settlements");
	}

	sscl::tests::expectCompletedIntSettlement<CalleeIntInvoker>(
		firstSettlement,
		timerDelayShortMs);
	sscl::tests::expectCompletedIntSettlement<CalleeIntInvoker>(
		allSettlementsAfterFirst[0],
		timerDelayShortMs);
	sscl::tests::expectCompletedIntSettlement<CalleeIntInvoker>(
		allSettlementsAfterFirst[1],
		timerDelayMediumMs);
	sscl::tests::expectCompletedIntSettlement<CalleeIntInvoker>(
		allSettlementsAfterFirst[2],
		timerDelayLongMs);

	co_return;
}

CallerDriver runGroupTimerCancelLongAfterAwaitFirst(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::tests::CancelableDeadlineTimerRegistry &registry)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker invokerShort =
		waitCancelableDeadlineTimer(timerDelayShortMs, registry);
	CalleeIntInvoker invokerMedium =
		waitCancelableDeadlineTimer(timerDelayMediumMs, registry);
	CalleeIntInvoker invokerLong =
		waitCancelableDeadlineTimer(timerDelayLongMs, registry);

	group.add(invokerShort);
	group.add(invokerMedium);
	group.add(invokerLong);

	const auto testStart = Clock::now();

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstSettlement, allSettlementsAfterFirst] = co_await awaitFirst;

	if (&firstSettlement.invokerAs<CalleeIntInvoker>() != &invokerShort) {
		throw std::runtime_error("cancel test first settlement mismatch");
	}

	if (group.allInvokersSettled()) {
		throw std::runtime_error("cancel test all settled after await-first");
	}

	registry.cancel(timerDelayLongMs);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allSettlements = co_await awaitAll;

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

	sscl::tests::expectCompletedIntSettlement<CalleeIntInvoker>(
		allSettlements[0],
		timerDelayShortMs);
	sscl::tests::expectCompletedIntSettlement<CalleeIntInvoker>(
		allSettlements[1],
		timerDelayMediumMs);
	sscl::tests::expectCompletedIntSettlement<CalleeIntInvoker>(
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
	sscl::tests::PostingThreadSet threads;
};

} // namespace

TEST_F(GroupTimerTest, AwaitFirstReturnsShortestTimerAndAwaitAllWaitsForLongest)
{
	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return runGroupTimerRace(
					exceptionPtr,
					std::move(completion));
			}));
}

TEST_F(GroupTimerTest, CancelLongTimerAfterAwaitFirst)
{
	sscl::tests::CancelableDeadlineTimerRegistry registry;

	ASSERT_NO_THROW(
		sscl::tests::runNonViralPostingTask(
			threads.caller(),
			[&registry](
				std::exception_ptr &exceptionPtr,
				std::function<void()> completion)
			{
				return runGroupTimerCancelLongAfterAwaitFirst(
					exceptionPtr,
					std::move(completion),
					registry);
			}));
}
