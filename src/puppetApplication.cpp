#include <iostream>
#include <string_view>
#include <vector>

#include <spinscale/co/group.h>
#include <spinscale/puppetApplication.h>
#include <spinscale/componentThread.h>

namespace sscl {

namespace puppet_application_detail {

constexpr std::string_view noPuppetThreadsToStartLogMessage =
	"Mrntt: No puppet threads to start";
constexpr std::string_view noPuppetThreadsToPauseLogMessage =
	"Mrntt: No puppet threads to pause";
constexpr std::string_view noPuppetThreadsToResumeLogMessage =
	"Mrntt: No puppet threads to resume";
constexpr std::string_view noPuppetThreadsToExitLogMessage =
	"Mrntt: No puppet threads to exit";

using PuppetLifetimeInvoker = PuppetThread::ViralThreadLifetimeMgmtInvoker;
using PuppetLifetimeGroup = co::Group<PuppetLifetimeInvoker>;

void addAllPuppetLifetimeInvokersToGroup(
	PuppetLifetimeGroup &group,
	std::vector<PuppetLifetimeInvoker> &invokers,
	const std::vector<std::shared_ptr<PuppetThread>> &componentThreads,
	PuppetThread::ThreadOp threadOp)
{
	invokers.reserve(componentThreads.size());

	for (const auto &thread : componentThreads)
	{
		switch (threadOp)
		{
		case PuppetThread::ThreadOp::START:
			invokers.emplace_back(thread->startThreadAReq());
			break;
		case PuppetThread::ThreadOp::PAUSE:
			invokers.emplace_back(thread->pauseThreadAReq());
			break;
		case PuppetThread::ThreadOp::RESUME:
			invokers.emplace_back(thread->resumeThreadAReq());
			break;
		case PuppetThread::ThreadOp::EXIT:
			invokers.emplace_back(thread->exitThreadAReq());
			break;
		case PuppetThread::ThreadOp::JOLT:
			invokers.emplace_back(thread->joltThreadAReq(thread));
			break;
		default:
			throw std::runtime_error(
				std::string(__func__) + ": Invalid thread operation");
		}

		group.add(invokers.back());
	}
}

co::NonViralNonPostingInvoker genericAllPuppetThreadsLifetimeOpCReq(
	const std::vector<std::shared_ptr<PuppetThread>> &componentThreads,
	PuppetThread::ThreadOp threadOp,
	std::string_view emptyThreadsLogMessage,
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	if (componentThreads.empty())
	{
		std::cout << emptyThreadsLogMessage << "\n";
		co_return;
	}

	PuppetLifetimeGroup group;
	std::vector<PuppetLifetimeInvoker> invokers;

	addAllPuppetLifetimeInvokersToGroup(
		group, invokers, componentThreads, threadOp);

	co_await group.getAwaitAllSettlementsInvoker();
	group.checkForAndReThrowGroupExceptions();

	co_return;
}

} // namespace puppet_application_detail

PuppetApplication::PuppetApplication(
	const std::vector<std::shared_ptr<PuppetThread>> &threads)
:	componentThreads(threads)
{
}

co::NonViralNonPostingInvoker PuppetApplication::joltAllPuppetThreadsCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	if (threadsHaveBeenJolted)
	{
		std::cout << "Mrntt: All puppet threads already JOLTed. "
			<< "Skipping JOLT request." << "\n";
		co_return;
	}

	if (componentThreads.empty())
	{
		threadsHaveBeenJolted = true;
		co_return;
	}

	puppet_application_detail::PuppetLifetimeGroup group;
	std::vector<puppet_application_detail::PuppetLifetimeInvoker> invokers;

	puppet_application_detail::addAllPuppetLifetimeInvokersToGroup(
		group, invokers, componentThreads, PuppetThread::ThreadOp::JOLT);

	co_await group.getAwaitAllSettlementsInvoker();
	group.checkForAndReThrowGroupExceptions();

	threadsHaveBeenJolted = true;
	co_return;
}

co::NonViralNonPostingInvoker PuppetApplication::startAllPuppetThreadsCReq(
	std::exception_ptr &exceptionPtr, std::function<void()> callback)
{
	return puppet_application_detail::genericAllPuppetThreadsLifetimeOpCReq(
		componentThreads, PuppetThread::ThreadOp::START,
		puppet_application_detail::noPuppetThreadsToStartLogMessage,
		exceptionPtr, callback);
}

co::NonViralNonPostingInvoker PuppetApplication::pauseAllPuppetThreadsCReq(
	std::exception_ptr &exceptionPtr, std::function<void()> callback)
{
	return puppet_application_detail::genericAllPuppetThreadsLifetimeOpCReq(
		componentThreads, PuppetThread::ThreadOp::PAUSE,
		puppet_application_detail::noPuppetThreadsToPauseLogMessage,
		exceptionPtr, callback);
}

co::NonViralNonPostingInvoker PuppetApplication::resumeAllPuppetThreadsCReq(
	std::exception_ptr &exceptionPtr, std::function<void()> callback)
{
	return puppet_application_detail::genericAllPuppetThreadsLifetimeOpCReq(
		componentThreads, PuppetThread::ThreadOp::RESUME,
		puppet_application_detail::noPuppetThreadsToResumeLogMessage,
		exceptionPtr, callback);
}

co::NonViralNonPostingInvoker PuppetApplication::exitAllPuppetThreadsCReq(
	[[maybe_unused]] std::exception_ptr &exceptionPtr,
	[[maybe_unused]] std::function<void()> callback)
{
	if (componentThreads.empty())
	{
		std::cout << puppet_application_detail::noPuppetThreadsToExitLogMessage
			<< "\n";
		co_return;
	}

	puppet_application_detail::PuppetLifetimeGroup group;
	std::vector<puppet_application_detail::PuppetLifetimeInvoker> invokers;

	puppet_application_detail::addAllPuppetLifetimeInvokersToGroup(
		group, invokers, componentThreads, PuppetThread::ThreadOp::EXIT);

	co_await group.getAwaitAllSettlementsInvoker();
	group.checkForAndReThrowGroupExceptions();

	for (auto &thread : componentThreads) {
		thread->thread.join();
	}

	co_return;
}

void PuppetApplication::distributeAndPinThreadsAcrossCpus()
{
	int cpuCount = ComponentThread::getAvailableCpuCount();

	// Distribute and pin threads across CPUs
	int threadIndex = 0;
	for (auto& thread : componentThreads)
	{
		int targetCpu = threadIndex % cpuCount;
		thread->pinToCpu(targetCpu);
		++threadIndex;
	}

	std::cout << __func__ << ": Distributed " << threadIndex << " threads "
		<< "across " << cpuCount << " CPUs\n";
}

} // namespace sscl
