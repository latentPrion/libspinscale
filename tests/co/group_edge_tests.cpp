#include <boostAsioLinkageFix.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>

#include <spinscale/co/group.h>
#include <spinscale/componentThread.h>

#include <support/groupAssertions.h>
#include <support/threadHarness.h>
#include <support/timerAwaiters.h>

namespace {

constexpr int delayShortMs = 50;
constexpr int delayMediumMs = 200;
constexpr int delayLongMs = 500;
constexpr int delayAddWhileSuspendedProbeMs = 80;
constexpr int expectedNonStdThrowValue = 42;
constexpr int wave2ImmediateSettlementLabel = 1000;
constexpr const char *expectedThrowMessage =
	"group_edge_test intentional failure";
constexpr std::array<int, 3> immediateSettlementValues = {10, 20, 30};

using CallerDriver =
	sscl::tests::RoleNonViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLER>;

using CalleeIntInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLEE,
		int>;

using CalleeVoidInvoker =
	sscl::tests::RoleViralPostingInvoker<
		sscl::tests::PostingThreadRole::CALLEE,
		void>;

CalleeIntInvoker waitAndReturnLabel(int timerLabelMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			timerLabelMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	co_return timerLabelMilliseconds;
}

CalleeIntInvoker waitThenThrowAfterDelay(int delayMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			delayMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	throw std::runtime_error(expectedThrowMessage);
}

CalleeIntInvoker waitThenThrowIntAfterDelay(int delayMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			delayMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	throw expectedNonStdThrowValue;
}

CalleeIntInvoker returnLabelImmediately(int label)
{
	co_return label;
}

void requireImmediateSettlementValues(
	const std::vector<sscl::co::Group::SettlementDescriptor>& descriptors)
{
	if (descriptors.size() != immediateSettlementValues.size()) {
		throw std::runtime_error("immediate settlement count mismatch");
	}

	std::array<int, immediateSettlementValues.size()> actualValues;
	std::transform(
		descriptors.begin(),
		descriptors.end(),
		actualValues.begin(),
		[](const sscl::co::Group::SettlementDescriptor& descriptor)
		{
			sscl::tests::requireCompletedSettlement(descriptor);
			return sscl::tests::completedIntValue(
				descriptor.invokerAs<CalleeIntInvoker>());
		});
	std::sort(actualValues.begin(), actualValues.end());

	if (actualValues != immediateSettlementValues) {
		throw std::runtime_error("immediate settlement values mismatch");
	}
}

CalleeVoidInvoker voidMemberAfterDelay(int delayMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			delayMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	co_return;
}

CalleeIntInvoker waitRecordThreadAndReturnLabel(
	int timerLabelMilliseconds,
	sscl::tests::CrossThreadTrace &trace)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			timerLabelMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	trace.recordCalleeExecutionThread();
	co_return timerLabelMilliseconds;
}

sscl::co::ViralNonPostingInvoker<void> waitOnCallerThread(int delayMilliseconds)
{
	const boost::system::error_code waitError =
		co_await sscl::tests::DeadlineTimerAwaiter{
			sscl::ComponentThread::getSelf()->getIoContext(),
			delayMilliseconds};
	sscl::tests::throwIfTimerWaitFailed(waitError);
	co_return;
}

CallerDriver mixedSuccessAndFailureAwaitFirstThenAll(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker successInvoker = waitAndReturnLabel(1);
	CalleeIntInvoker failureInvoker = waitThenThrowAfterDelay(delayShortMs);

	group.add(successInvoker);
	group.add(failureInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	if (firstDescriptor.type
		== sscl::co::Group::SettlementDescriptor::TypeE::COMPLETED) {
		sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
			firstDescriptor,
			1);
	}
	else if (firstDescriptor.type
		== sscl::co::Group::SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
		sscl::tests::requireRuntimeErrorSettlement(
			firstDescriptor,
			expectedThrowMessage);
	}
	else {
		throw std::runtime_error("first settlement has unexpected type");
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 2 || allAfterFirst.size() != 2) {
		throw std::runtime_error("mixed settlement count mismatch");
	}

	std::size_t completedCount = 0;
	std::size_t exceptionCount = 0;

	for (auto &descriptor : allDescriptors) {
		if (descriptor.type
			== sscl::co::Group::SettlementDescriptor::TypeE::COMPLETED) {
			++completedCount;
			sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
				descriptor,
				1);
		}
		else if (descriptor.type
			== sscl::co::Group::SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
			++exceptionCount;
			sscl::tests::requireRuntimeErrorSettlement(
				descriptor,
				expectedThrowMessage);
		}
	}

	if (completedCount != 1 || exceptionCount != 1) {
		throw std::runtime_error("mixed settlement type counts mismatch");
	}

	co_return;
}

CallerDriver singleMemberAwaitFirstThenAll(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker onlyInvoker = waitAndReturnLabel(delayShortMs);
	group.add(onlyInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptor,
		delayShortMs);

	if (!group.allInvokersSettled() || allAfterFirst.size() != 1) {
		throw std::runtime_error("single member state mismatch");
	}

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("single member await-all count mismatch");
	}

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allDescriptors[0],
		delayShortMs);
	co_return;
}

CallerDriver allCompleteBeforeCoAwait(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker invokerTen = returnLabelImmediately(10);
	CalleeIntInvoker invokerTwenty = returnLabelImmediately(20);
	CalleeIntInvoker invokerThirty = returnLabelImmediately(30);

	group.add(invokerTen);
	group.add(invokerTwenty);
	group.add(invokerThirty);

	co_await waitOnCallerThread(delayShortMs);

	if (!group.allInvokersSettled() || !group.firstInvokerSettled()) {
		throw std::runtime_error("immediate group did not settle before await");
	}

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;
	sscl::tests::requireCompletedSettlement(firstDescriptor);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	requireImmediateSettlementValues(allAfterFirst);
	requireImmediateSettlementValues(allDescriptors);

	co_return;
}

std::jthread startAddWhileGroupAwaiterSuspendedProbe(
	sscl::co::Group &group,
	CalleeIntInvoker &lateInvoker,
	std::atomic<bool> &groupIsAwaitingAll,
	std::atomic<bool> &addWasRejected)
{
	return std::jthread(
		[&]()
		{
			while (!groupIsAwaitingAll.load(std::memory_order_acquire)) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			std::this_thread::sleep_for(
				std::chrono::milliseconds(delayAddWhileSuspendedProbeMs));

			boost::asio::post(
				sscl::tests::ThreadRegistry::ioContext(
					sscl::tests::PostingThreadRole::CALLER),
				[&]()
				{
					try {
						group.add(lateInvoker);
					}
					catch (const std::runtime_error &) {
						addWasRejected.store(true, std::memory_order_release);
					}
				});
		});
}

CallerDriver addWhileAwaitAllSuspended(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	std::atomic<bool> groupIsAwaitingAll{false};
	std::atomic<bool> addWasRejected{false};

	CalleeIntInvoker slowInvokerA = waitAndReturnLabel(delayLongMs);
	CalleeIntInvoker slowInvokerB = waitAndReturnLabel(delayLongMs);
	CalleeIntInvoker lateInvoker = waitAndReturnLabel(99);

	group.add(slowInvokerA);
	group.add(slowInvokerB);

	std::jthread addProbeThread = startAddWhileGroupAwaiterSuspendedProbe(
		group,
		lateInvoker,
		groupIsAwaitingAll,
		addWasRejected);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	groupIsAwaitingAll.store(true, std::memory_order_release);
	co_await awaitAll;

	addProbeThread.join();

	if (!addWasRejected.load(std::memory_order_acquire)) {
		throw std::runtime_error("expected add while suspended to throw");
	}

	co_return;
}

CallerDriver awaitAllOnlyMixedOutcomes(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker successInvoker = returnLabelImmediately(7);
	CalleeIntInvoker failureInvoker = waitThenThrowAfterDelay(delayShortMs);

	group.add(successInvoker);
	group.add(failureInvoker);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptors = co_await awaitAll;

	if (allDescriptors.size() != 2) {
		throw std::runtime_error("await-all-only count mismatch");
	}

	std::size_t completedCount = 0;
	std::size_t exceptionCount = 0;

	for (auto &descriptor : allDescriptors) {
		if (descriptor.type
			== sscl::co::Group::SettlementDescriptor::TypeE::COMPLETED) {
			++completedCount;
			sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
				descriptor,
				7);
		}
		else if (descriptor.type
			== sscl::co::Group::SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
			++exceptionCount;
			sscl::tests::requireRuntimeErrorSettlement(
				descriptor,
				expectedThrowMessage);
		}
	}

	if (completedCount != 1 || exceptionCount != 1) {
		throw std::runtime_error("await-all-only mixed counts mismatch");
	}

	co_return;
}

CallerDriver checkForAndReThrowGroupExceptions(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker failureInvoker = waitThenThrowAfterDelay(delayShortMs);
	group.add(failureInvoker);

	(void)co_await group.getAwaitAllSettlementsInvoker();

	try {
		group.checkForAndReThrowGroupExceptions();
	}
	catch (const std::runtime_error &aggregateError) {
		if (std::string(aggregateError.what()).find(expectedThrowMessage)
			== std::string::npos) {
			throw std::runtime_error("aggregate message missing callee text");
		}
		co_return;
	}

	throw std::runtime_error("expected aggregate group exception");
}

CallerDriver emptyGroupAwaitAllThrows(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;

	try {
		(void)co_await group.getAwaitAllSettlementsInvoker();
	}
	catch (const std::runtime_error &runtimeError) {
		sscl::tests::requireEmptyGroupError(runtimeError);
		co_return;
	}

	throw std::runtime_error("expected empty group await-all to throw");
}

CallerDriver emptyGroupAwaitFirstThrows(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;

	try {
		(void)co_await group.getAwaitFirstSettlementInvoker();
	}
	catch (const std::runtime_error &runtimeError) {
		sscl::tests::requireEmptyGroupError(runtimeError);
		co_return;
	}

	throw std::runtime_error("expected empty group await-first to throw");
}

CallerDriver wrongAwaitInvokerOrder(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker shortInvoker = waitAndReturnLabel(delayShortMs);
	CalleeIntInvoker mediumInvoker = waitAndReturnLabel(delayMediumMs);

	group.add(shortInvoker);
	group.add(mediumInvoker);

	auto awaitFirstHandle = group.getAwaitFirstSettlementInvoker();
	auto awaitAllHandle = group.getAwaitAllSettlementsInvoker();

	auto &allDescriptors = co_await awaitAllHandle;
	if (allDescriptors.size() != 2) {
		throw std::runtime_error("wrong-order await-all count mismatch");
	}

	auto [firstDescriptor, allAfterFirst] = co_await awaitFirstHandle;
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptor,
		sscl::tests::completedIntValue(
			firstDescriptor.invokerAs<CalleeIntInvoker>()));

	if (!group.firstInvokerSettled() || allAfterFirst.size() != 2) {
		throw std::runtime_error("wrong-order await-first state mismatch");
	}

	co_return;
}

CallerDriver doubleCoAwaitSameAwaitFirst(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker memberInvoker = returnLabelImmediately(delayShortMs);
	group.add(memberInvoker);

	co_await waitOnCallerThread(delayShortMs);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptorA, allAfterFirstA] = co_await awaitFirst;
	auto [firstDescriptorB, allAfterFirstB] = co_await awaitFirst;

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptorA,
		delayShortMs);
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptorB,
		delayShortMs);

	if (&firstDescriptorA.invokerAs<CalleeIntInvoker>()
		!= &firstDescriptorB.invokerAs<CalleeIntInvoker>()) {
		throw std::runtime_error("double await-first descriptor mismatch");
	}

	if (allAfterFirstA.size() != allAfterFirstB.size()) {
		throw std::runtime_error("double await-first snapshot mismatch");
	}

	co_return;
}

CallerDriver doubleCoAwaitSameAwaitAll(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker memberInvoker = waitAndReturnLabel(delayShortMs);
	group.add(memberInvoker);

	auto awaitAll = group.getAwaitAllSettlementsInvoker();
	auto &allDescriptorsA = co_await awaitAll;
	auto &allDescriptorsB = co_await awaitAll;

	if (allDescriptorsA.size() != 1 || allDescriptorsB.size() != 1) {
		throw std::runtime_error("double await-all count mismatch");
	}

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allDescriptorsA[0],
		delayShortMs);
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		allDescriptorsB[0],
		delayShortMs);
	co_return;
}

CallerDriver twoAwaitFirstHandlesSequentially(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker shortInvoker = waitAndReturnLabel(delayShortMs);
	CalleeIntInvoker mediumInvoker = waitAndReturnLabel(delayMediumMs);

	group.add(shortInvoker);
	group.add(mediumInvoker);

	auto awaitFirstA = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptorA, allAfterFirstA] = co_await awaitFirstA;
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptorA,
		delayShortMs);

	auto awaitFirstB = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptorB, allAfterFirstB] = co_await awaitFirstB;
	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptorB,
		delayShortMs);

	if (&firstDescriptorA.invokerAs<CalleeIntInvoker>()
		!= &firstDescriptorB.invokerAs<CalleeIntInvoker>()) {
		throw std::runtime_error("sticky first settlement mismatch");
	}

	(void)co_await group.getAwaitAllSettlementsInvoker();
	(void)allAfterFirstA;
	(void)allAfterFirstB;
	co_return;
}

CallerDriver addSecondWaveAfterAwaitAll(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker wave1MemberA = waitAndReturnLabel(delayLongMs);
	CalleeIntInvoker wave1MemberB = waitAndReturnLabel(delayLongMs);

	group.add(wave1MemberA);
	group.add(wave1MemberB);
	(void)co_await group.getAwaitAllSettlementsInvoker();

	CalleeIntInvoker wave2Immediate =
		returnLabelImmediately(wave2ImmediateSettlementLabel);
	CalleeIntInvoker wave2Slow = waitAndReturnLabel(delayMediumMs);

	group.add(wave2Immediate);
	group.add(wave2Slow);

	co_await waitOnCallerThread(delayShortMs);

	if (sscl::tests::completedIntValue(wave2Immediate)
		!= wave2ImmediateSettlementLabel) {
		throw std::runtime_error("wave-2 immediate member did not complete");
	}

	if (group.allInvokersSettled()) {
		throw std::runtime_error("wave-2 slow member should still be in flight");
	}

	auto &allDescriptors =
		co_await group.getAwaitAllSettlementsInvoker();

	if (allDescriptors.size() != 4) {
		throw std::runtime_error("expected four settlements after second wave");
	}

	co_return;
}

CallerDriver shortTimerAddedAfterLongStillWinsRace(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker longInvoker = waitAndReturnLabel(delayLongMs);
	CalleeIntInvoker shortInvoker = waitAndReturnLabel(delayShortMs);

	group.add(longInvoker);
	group.add(shortInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptor,
		delayShortMs);

	if (&firstDescriptor.invokerAs<CalleeIntInvoker>() != &shortInvoker) {
		throw std::runtime_error("short timer should win first settlement");
	}

	(void)co_await group.getAwaitAllSettlementsInvoker();
	(void)allAfterFirst;
	co_return;
}

CallerDriver nonStdExceptionSettlement(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker failureInvoker = waitThenThrowIntAfterDelay(delayShortMs);
	group.add(failureInvoker);

	auto &allDescriptors = co_await group.getAwaitAllSettlementsInvoker();

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("non-std exception count mismatch");
	}

	sscl::tests::requireIntExceptionSettlement(
		allDescriptors[0],
		expectedNonStdThrowValue);

	try {
		group.checkForAndReThrowGroupExceptions();
	}
	catch (const std::runtime_error &) {
		co_return;
	}

	throw std::runtime_error("expected aggregate for non-std exception");
}

CallerDriver voidViralMemberInGroup(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeVoidInvoker voidInvoker = voidMemberAfterDelay(delayShortMs);
	group.add(voidInvoker);

	auto &allDescriptors = co_await group.getAwaitAllSettlementsInvoker();

	if (allDescriptors.size() != 1) {
		throw std::runtime_error("void group count mismatch");
	}

	if (allDescriptors[0].type
		!= sscl::co::Group::SettlementDescriptor::TypeE::COMPLETED) {
		throw std::runtime_error("void member did not complete");
	}

	co_return;
}

CallerDriver returnValuesRemainReadableAfterAwaitFirst(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker slowInvoker = waitAndReturnLabel(delayLongMs);
	CalleeIntInvoker fastInvoker = waitAndReturnLabel(delayShortMs);

	group.add(slowInvoker);
	group.add(fastInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptor,
		delayShortMs);

	const int fastLabelFromDescriptor = sscl::tests::completedIntValue(
		firstDescriptor.invokerAs<CalleeIntInvoker>());
	const int fastLabelFromLocal =
		sscl::tests::completedIntValue(fastInvoker);

	if (fastLabelFromDescriptor != fastLabelFromLocal) {
		throw std::runtime_error("descriptor/local return value mismatch");
	}

	if (allAfterFirst.size() != 2) {
		throw std::runtime_error("expected two settlement slots");
	}

	(void)co_await group.getAwaitAllSettlementsInvoker();
	co_return;
}

CallerDriver groupMemberRunsOnCalleeAndAwaitResumesOnCaller(
	std::exception_ptr &exceptionPtr,
	std::function<void()> completion,
	sscl::tests::CrossThreadTrace &trace)
{
	(void)exceptionPtr;
	(void)completion;

	sscl::co::Group group;
	CalleeIntInvoker memberInvoker = waitRecordThreadAndReturnLabel(
		delayShortMs,
		trace);
	group.add(memberInvoker);

	auto awaitFirst = group.getAwaitFirstSettlementInvoker();
	auto [firstDescriptor, allAfterFirst] = co_await awaitFirst;
	trace.recordAwaitResumeThread();

	sscl::tests::requireCompletedIntSettlement<CalleeIntInvoker>(
		firstDescriptor,
		delayShortMs);

	if (allAfterFirst.size() != 1) {
		throw std::runtime_error("cross-thread group trace count mismatch");
	}

	co_return;
}

class GroupEdgeTest
:	public ::testing::Test
{
protected:
	template <typename Factory>
	void runScenario(Factory &&factory)
	{
		ASSERT_NO_THROW(
			sscl::tests::runNonViralPostingTask(
				threads.caller(),
				std::forward<Factory>(factory)));
	}

	sscl::tests::PostingThreadSet threads;
};

} // namespace

#define RUN_GROUP_EDGE_SCENARIO(testName, functionName) \
	TEST_F(GroupEdgeTest, testName) \
	{ \
		runScenario( \
			[]( \
				std::exception_ptr &exceptionPtr, \
				std::function<void()> completion) \
			{ \
				return functionName(exceptionPtr, std::move(completion)); \
			}); \
	}

RUN_GROUP_EDGE_SCENARIO(
	MixedSuccessAndFailureAwaitFirstThenAll,
	mixedSuccessAndFailureAwaitFirstThenAll)
RUN_GROUP_EDGE_SCENARIO(
	SingleMemberAwaitFirstThenAll,
	singleMemberAwaitFirstThenAll)
RUN_GROUP_EDGE_SCENARIO(AllCompleteBeforeCoAwait, allCompleteBeforeCoAwait)
RUN_GROUP_EDGE_SCENARIO(AddWhileAwaitAllSuspended, addWhileAwaitAllSuspended)
RUN_GROUP_EDGE_SCENARIO(AwaitAllOnlyMixedOutcomes, awaitAllOnlyMixedOutcomes)
RUN_GROUP_EDGE_SCENARIO(
	CheckForAndReThrowGroupExceptions,
	checkForAndReThrowGroupExceptions)
RUN_GROUP_EDGE_SCENARIO(EmptyGroupAwaitAllThrows, emptyGroupAwaitAllThrows)
RUN_GROUP_EDGE_SCENARIO(EmptyGroupAwaitFirstThrows, emptyGroupAwaitFirstThrows)
RUN_GROUP_EDGE_SCENARIO(WrongAwaitInvokerOrder, wrongAwaitInvokerOrder)
RUN_GROUP_EDGE_SCENARIO(DoubleCoAwaitSameAwaitFirst, doubleCoAwaitSameAwaitFirst)
RUN_GROUP_EDGE_SCENARIO(DoubleCoAwaitSameAwaitAll, doubleCoAwaitSameAwaitAll)
RUN_GROUP_EDGE_SCENARIO(
	TwoAwaitFirstHandlesSequentially,
	twoAwaitFirstHandlesSequentially)
RUN_GROUP_EDGE_SCENARIO(AddSecondWaveAfterAwaitAll, addSecondWaveAfterAwaitAll)
RUN_GROUP_EDGE_SCENARIO(
	ShortTimerAddedAfterLongStillWinsRace,
	shortTimerAddedAfterLongStillWinsRace)
RUN_GROUP_EDGE_SCENARIO(NonStdExceptionSettlement, nonStdExceptionSettlement)
RUN_GROUP_EDGE_SCENARIO(VoidViralMemberInGroup, voidViralMemberInGroup)
RUN_GROUP_EDGE_SCENARIO(
	ReturnValuesRemainReadableAfterAwaitFirst,
	returnValuesRemainReadableAfterAwaitFirst)

TEST_F(GroupEdgeTest, SuspendingMemberRunsOnCalleeAndAwaitResumesOnCaller)
{
	sscl::tests::CrossThreadTrace trace;

	runScenario(
		[&trace](
			std::exception_ptr &exceptionPtr,
			std::function<void()> completion)
		{
			return groupMemberRunsOnCalleeAndAwaitResumesOnCaller(
				exceptionPtr,
				std::move(completion),
				trace);
		});

	EXPECT_EQ(trace.calleeExecutionThread(), threads.callee().osThreadId());
	EXPECT_EQ(trace.awaitResumeThread(), threads.caller().osThreadId());
	EXPECT_NE(trace.calleeExecutionThread(), trace.awaitResumeThread());
}

TEST_F(GroupEdgeTest, NonViralVoidGroupTemplateInstantiates)
{
	GTEST_SKIP()
		<< "NonViralPostingInvoker does not satisfy Group's awaitable concept.";
}

TEST_F(GroupEdgeTest, EarlyInvokerDestructionIsUnsupported)
{
	GTEST_SKIP()
		<< "Destroying a member invoker before group settlement completes is undefined.";
}

TEST_F(GroupEdgeTest, OverlappingGroupWaitsAssertInDebug)
{
	GTEST_SKIP()
		<< "Overlapping group co_await is debug-assert behavior.";
}
