#include <iostream>
#include <spinscale/asynchronousContinuation.h>
#include <spinscale/asynchronousLoop.h>
#include <spinscale/callback.h>
#include <spinscale/puppetApplication.h>
#include <spinscale/componentThread.h>

namespace sscl {

PuppetApplication::PuppetApplication(
	const std::vector<std::shared_ptr<PuppetThread>> &threads)
:	componentThreads(threads)
{
}

class PuppetApplication::PuppetThreadLifetimeMgmtOp
:	public NonPostedAsynchronousContinuation<puppetThreadLifetimeMgmtOpCbFn>
{
public:
	PuppetThreadLifetimeMgmtOp(
		PuppetApplication &parent, unsigned int nThreads,
		Callback<puppetThreadLifetimeMgmtOpCbFn> callback)
	:	NonPostedAsynchronousContinuation<puppetThreadLifetimeMgmtOpCbFn>(callback),
	loop(nThreads),
	parent(parent)
	{}

public:
	AsynchronousLoop	loop;
	PuppetApplication &parent;

public:
	void joltAllPuppetThreadsReq1(
		[[maybe_unused]] std::shared_ptr<PuppetThreadLifetimeMgmtOp> context
		)
	{
		loop.incrementSuccessOrFailureDueTo(true);
		if (!loop.isComplete()) {
			return;
		}

		parent.threadsHaveBeenJolted = true;
		callOriginalCb();
	}

	void executeGenericOpOnAllPuppetThreadsReq1(
		[[maybe_unused]] std::shared_ptr<PuppetThreadLifetimeMgmtOp> context
		)
	{
		loop.incrementSuccessOrFailureDueTo(true);
		if (!loop.isComplete()) {
			return;
		}

		callOriginalCb();
	}

	void exitAllPuppetThreadsReq1(
		[[maybe_unused]] std::shared_ptr<PuppetThreadLifetimeMgmtOp> context
		)
	{
		loop.incrementSuccessOrFailureDueTo(true);
		if (!loop.isComplete()) {
			return;
		}

		for (auto& thread : parent.componentThreads) {
			thread->thread.join();
		}

		callOriginalCb();
	}
};

void PuppetApplication::joltAllPuppetThreadsReq(
	Callback<puppetThreadLifetimeMgmtOpCbFn> callback
	)
{
	if (threadsHaveBeenJolted)
	{
		std::cout << "Mrntt: All puppet threads already JOLTed. "
			<< "Skipping JOLT request." << "\n";
		callback.callbackFn();
		return;
	}

	// If no threads, set flag and call callback immediately
	if (componentThreads.size() == 0 && callback.callbackFn)
	{
		threadsHaveBeenJolted = true;
		callback.callbackFn();
		return;
	}

	// Create a counter to track when all threads have been jolted
	auto request = std::make_shared<PuppetThreadLifetimeMgmtOp>(
		*this, componentThreads.size(), callback);

	for (auto& thread : componentThreads)
	{
		thread->joltThreadReq(
			thread,
			{request, std::bind(
				&PuppetThreadLifetimeMgmtOp::joltAllPuppetThreadsReq1,
				request.get(), request)});
	}
}

void PuppetApplication::startAllPuppetThreadsReq(
	Callback<puppetThreadLifetimeMgmtOpCbFn> callback
	)
{
	// If no threads, call callback immediately
	if (componentThreads.size() == 0 && callback.callbackFn)
	{
		callback.callbackFn();
		return;
	}

	// Create a counter to track when all threads have started
	auto request = std::make_shared<PuppetThreadLifetimeMgmtOp>(
		*this, componentThreads.size(), callback);

	for (auto& thread : componentThreads)
	{
		thread->startThreadReq(
			{request, std::bind(
				&PuppetThreadLifetimeMgmtOp::executeGenericOpOnAllPuppetThreadsReq1,
				request.get(), request)});
	}
}

void PuppetApplication::pauseAllPuppetThreadsReq(
	Callback<puppetThreadLifetimeMgmtOpCbFn> callback
	)
{
	// If no threads, call callback immediately
	if (componentThreads.size() == 0 && callback.callbackFn)
	{
		callback.callbackFn();
		return;
	}

	// Create a counter to track when all threads have paused
	auto request = std::make_shared<PuppetThreadLifetimeMgmtOp>(
		*this, componentThreads.size(), callback);

	for (auto& thread : componentThreads)
	{
		thread->pauseThreadReq(
			{request, std::bind(
				&PuppetThreadLifetimeMgmtOp::executeGenericOpOnAllPuppetThreadsReq1,
				request.get(), request)});
	}
}

void PuppetApplication::resumeAllPuppetThreadsReq(
	Callback<puppetThreadLifetimeMgmtOpCbFn> callback
	)
{
	// If no threads, call callback immediately
	if (componentThreads.size() == 0 && callback.callbackFn)
	{
		callback.callbackFn();
		return;
	}

	// Create a counter to track when all threads have resumed
	auto request = std::make_shared<PuppetThreadLifetimeMgmtOp>(
		*this, componentThreads.size(), callback);

	for (auto& thread : componentThreads)
	{
		thread->resumeThreadReq(
			{request, std::bind(
				&PuppetThreadLifetimeMgmtOp::executeGenericOpOnAllPuppetThreadsReq1,
				request.get(), request)});
	}
}

void PuppetApplication::exitAllPuppetThreadsReq(
	Callback<puppetThreadLifetimeMgmtOpCbFn> callback
	)
{
	// If no threads, call callback immediately
	if (componentThreads.size() == 0 && callback.callbackFn)
	{
		callback.callbackFn();
		return;
	}

	// Create a counter to track when all threads have exited
	auto request = std::make_shared<PuppetThreadLifetimeMgmtOp>(
		*this, componentThreads.size(), callback);

	for (auto& thread : componentThreads)
	{
		thread->exitThreadReq(
			{request, std::bind(
				&PuppetThreadLifetimeMgmtOp::executeGenericOpOnAllPuppetThreadsReq1,
				request.get(), request)});
	}
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
