#include <iostream>
#include <string_view>
#include <vector>

#include <spinscale/co/group.h>
#include <spinscale/puppetApplication.h>
#include <spinscale/componentThread.h>

namespace sscl {

namespace {

constexpr std::string_view noPuppetThreadsToStartLogMessage =
	"Mrntt: No puppet threads to start";
constexpr std::string_view noPuppetThreadsToPauseLogMessage =
	"Mrntt: No puppet threads to pause";
constexpr std::string_view noPuppetThreadsToResumeLogMessage =
	"Mrntt: No puppet threads to resume";
constexpr std::string_view noPuppetThreadsToExitLogMessage =
	"Mrntt: No puppet threads to exit";

} // namespace

PuppetApplication::PuppetApplication(
	const std::vector<std::shared_ptr<PuppetThread>> &threads)
:	componentThreads(threads)
{
}

void PuppetApplication::addAllPuppetLifetimeInvokersToGroup(
	PuppetLifetimeMgmtGroup &group,
	std::vector<PuppetLifetimeMgmtInvoker> &invokers,
	PuppetThread::ThreadOp threadOp) const
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

co::ViralNonPostingInvoker<void>
PuppetApplication::joltAllPuppetThreadsCReq()
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

	PuppetLifetimeMgmtGroup group;
	std::vector<PuppetLifetimeMgmtInvoker> invokers;

	addAllPuppetLifetimeInvokersToGroup(
		group, invokers, PuppetThread::ThreadOp::JOLT);
	co_await group.getAwaitAllSettlementsInvoker();
	group.checkForAndReThrowGroupExceptions();

	threadsHaveBeenJolted = true;
	co_return;
}

co::ViralNonPostingInvoker<void>
PuppetApplication::allPuppetThreadsLifetimeOpCReq(
	PuppetThread::ThreadOp threadOp,
	std::string_view emptyThreadsLogMessage)
{
	if (componentThreads.empty())
	{
		std::cout << emptyThreadsLogMessage << "\n";
		co_return;
	}

	PuppetLifetimeMgmtGroup group;
	std::vector<PuppetLifetimeMgmtInvoker> invokers;

	addAllPuppetLifetimeInvokersToGroup(group, invokers, threadOp);
	co_await group.getAwaitAllSettlementsInvoker();
	group.checkForAndReThrowGroupExceptions();

	co_return;
}

co::ViralNonPostingInvoker<void>
PuppetApplication::startAllPuppetThreadsCReq()
{
	return allPuppetThreadsLifetimeOpCReq(
		PuppetThread::ThreadOp::START,
		noPuppetThreadsToStartLogMessage);
}

co::ViralNonPostingInvoker<void>
PuppetApplication::pauseAllPuppetThreadsCReq()
{
	return allPuppetThreadsLifetimeOpCReq(
		PuppetThread::ThreadOp::PAUSE,
		noPuppetThreadsToPauseLogMessage);
}

co::ViralNonPostingInvoker<void>
PuppetApplication::resumeAllPuppetThreadsCReq()
{
	return allPuppetThreadsLifetimeOpCReq(
		PuppetThread::ThreadOp::RESUME,
		noPuppetThreadsToResumeLogMessage);
}

co::ViralNonPostingInvoker<void>
PuppetApplication::exitAllPuppetThreadsCReq()
{
	if (componentThreads.empty())
	{
		std::cout << noPuppetThreadsToExitLogMessage << "\n";
		co_return;
	}

	co_await allPuppetThreadsLifetimeOpCReq(
		PuppetThread::ThreadOp::EXIT,
		noPuppetThreadsToExitLogMessage);

	for (auto &thread : componentThreads) {
		thread->thread.join();
	}

	co_return;
}

void PuppetApplication::distributeAndPinThreadsAcrossCpus()
{
	int cpuCount = ComponentThread::getAvailableCpuCount();

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
