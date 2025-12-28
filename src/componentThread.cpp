#include <boostAsioLinkageFix.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <pthread.h>
#include <sched.h>
#include <boost/asio/io_service.hpp>
#include <spinscale/asynchronousContinuation.h>
#include <spinscale/callback.h>
#include <spinscale/callableTracer.h>
#include <spinscale/componentThread.h>
#include <spinscale/marionette.h>

namespace sscl {

namespace mrntt {
// Global variable to store the marionette thread ID
// Default value is 0, but should be set by application code via setMarionetteThreadId()
ThreadId marionetteThreadId = 0;

void setMarionetteThreadId(ThreadId id)
{
	marionetteThreadId = id;
}
} // namespace mrntt

} // namespace sscl

namespace sscl {

thread_local std::shared_ptr<ComponentThread> thisComponentThread;

namespace mrntt {
// Global marionette thread instance - defined here but initialized by application
std::shared_ptr<MarionetteThread> thread;
} // namespace mrntt

// Implementation of static method
std::shared_ptr<MarionetteThread> ComponentThread::getMrntt()
{
	return sscl::mrntt::thread;
}

void MarionetteThread::initializeTls(void)
{
	thisComponentThread = shared_from_this();
}

void PuppetThread::initializeTls(void)
{
	thisComponentThread = shared_from_this();
}

bool ComponentThread::tlsInitialized(void)
{
	return thisComponentThread != nullptr;
}

const std::shared_ptr<ComponentThread> ComponentThread::getSelf(void)
{
	if (!thisComponentThread)
	{
		throw std::runtime_error(std::string(__func__)
			+ ": TLS not initialized");
	}

	return thisComponentThread;
}

class PuppetThread::ThreadLifetimeMgmtOp
:	public PostedAsynchronousContinuation<threadLifetimeMgmtOpCbFn>
{
public:
	ThreadLifetimeMgmtOp(
		const std::shared_ptr<ComponentThread> &caller,
		const std::shared_ptr<PuppetThread> &target,
		Callback<threadLifetimeMgmtOpCbFn> callback)
	:	PostedAsynchronousContinuation<threadLifetimeMgmtOpCbFn>(
			caller, callback),
	target(target)
	{}

public:
	const std::shared_ptr<PuppetThread> target;

public:
	void joltThreadReq1_posted(
		[[maybe_unused]] std::shared_ptr<ThreadLifetimeMgmtOp> context
		)
	{
		std::cout << __func__ << ": Thread '" << target->name << "': handling "
			"JOLT request."
			<< "\n";

		target->io_service.stop();
		callOriginalCb();
	}

	void startThreadReq1_posted(
		[[maybe_unused]] std::shared_ptr<ThreadLifetimeMgmtOp> context
		)
	{
		std::cout << __func__ << ": Thread '" << target->name << "': handling "
			"startThread."
			<< "\n";

		// Execute private setup sequence here
		// This is where each thread would implement its specific initialization

		callOriginalCb();
	}

	void exitThreadReq1_mainQueue_posted(
		[[maybe_unused]] std::shared_ptr<ThreadLifetimeMgmtOp> context
		)
	{
		std::cout << __func__ << ": Thread '" << target->name << "': handling "
			"exitThread (main queue)." << "\n";

		target->cleanup();
		target->io_service.stop();
		callOriginalCb();
	}

	void exitThreadReq1_pauseQueue_posted(
		[[maybe_unused]] std::shared_ptr<ThreadLifetimeMgmtOp> context
		)
	{
		std::cout << __func__ << ": Thread '" << target->name << "': handling "
			"exitThread (pause queue)."<< "\n";

		target->cleanup();
		target->pause_io_service.stop();
		target->io_service.stop();
		callOriginalCb();
	}

	void pauseThreadReq1_posted(
		[[maybe_unused]] std::shared_ptr<ThreadLifetimeMgmtOp> context
		)
	{
		std::cout << __func__ << ": Thread '" << target->name << "': handling "
			"pauseThread." << "\n";

		/* We have to invoke the callback here before moving on because 
		 * our next operation is going to block the thread, so it won't
		 * have a chance to invoke the callback until it's unblocked.
		 */
		callOriginalCb();
		target->pause_io_service.reset();
		target->pause_io_service.run();
	}

	void resumeThreadReq1_posted(
		[[maybe_unused]] std::shared_ptr<ThreadLifetimeMgmtOp> context
		)
	{
		std::cout << __func__ << ": Thread '" << target->name << "': handling "
			"resumeThread." << "\n";

		target->pause_io_service.stop();
		callOriginalCb();
	}
};

void ComponentThread::cleanup(void)
{
	this->keepLooping = false;
}

void PuppetThread::joltThreadReq(
	const std::shared_ptr<PuppetThread>& selfPtr,
	Callback<threadLifetimeMgmtOpCbFn> callback)
{
	/**	EXPLANATION:
	 * We can't use shared_from_this() here because JOLTing occurs prior to
	 * TLS being set up.
	 *
	 * We also can't use getSelf() as yet for the same reason: getSelf()
	 * requires TLS to be set up.
	 *
	 * To obtain a sh_ptr to the caller, we just supply the mrntt thread since
	 * JOLT is always invoked by the mrntt thread. The JOLT sequence that the
	 * CRT main() function invokes on the mrntt thread is special since it
	 * supplies cmdline args and envp.
	 *
	 * To obtain a sh_ptr to the target thread, we use the selfPtr parameter
	 * passed in by the caller.
	 */
	if (id == sscl::mrntt::marionetteThreadId)
	{
		throw std::runtime_error(std::string(__func__)
			+ ": invoked on mrntt thread");
	}

	std::shared_ptr<MarionetteThread> mrntt = sscl::mrntt::thread;

	auto request = std::make_shared<ThreadLifetimeMgmtOp>(
		mrntt, selfPtr, callback);

	this->getIoService().post(
		STC(std::bind(
			&ThreadLifetimeMgmtOp::joltThreadReq1_posted,
			request.get(), request)));
}

// Thread management method implementations
void PuppetThread::startThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback)
{
	std::shared_ptr<ComponentThread> caller = getSelf();
	auto request = std::make_shared<ThreadLifetimeMgmtOp>(
		caller, std::static_pointer_cast<PuppetThread>(shared_from_this()),
		callback);

	this->getIoService().post(
		STC(std::bind(
			&ThreadLifetimeMgmtOp::startThreadReq1_posted,
			request.get(), request)));
}

void PuppetThread::exitThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback)
{
	std::shared_ptr<ComponentThread> caller = getSelf();
	auto request = std::make_shared<ThreadLifetimeMgmtOp>(
		caller, std::static_pointer_cast<PuppetThread>(shared_from_this()),
		callback);

	this->getIoService().post(
		STC(std::bind(
			&ThreadLifetimeMgmtOp::exitThreadReq1_mainQueue_posted,
			request.get(), request)));

	pause_io_service.post(
		STC(std::bind(
			&ThreadLifetimeMgmtOp::exitThreadReq1_pauseQueue_posted,
			request.get(), request)));
}

void PuppetThread::pauseThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback)
{
	if (id == sscl::mrntt::marionetteThreadId)
	{
		throw std::runtime_error(std::string(__func__)
			+ ": invoked on mrntt thread");
	}

	std::shared_ptr<ComponentThread> caller = getSelf();
	auto request = std::make_shared<ThreadLifetimeMgmtOp>(
		caller, std::static_pointer_cast<PuppetThread>(shared_from_this()),
		callback);

	this->getIoService().post(
		STC(std::bind(
			&ThreadLifetimeMgmtOp::pauseThreadReq1_posted,
			request.get(), request)));
}

void PuppetThread::resumeThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback)
{
	if (id == sscl::mrntt::marionetteThreadId)
	{
		throw std::runtime_error(std::string(__func__)
			+ ": invoked on mrntt thread");
	}

	// Post to the pause_io_service to unblock the paused thread
	std::shared_ptr<ComponentThread> caller = getSelf();
	auto request = std::make_shared<ThreadLifetimeMgmtOp>(
		caller, std::static_pointer_cast<PuppetThread>(shared_from_this()),
		callback);

	pause_io_service.post(
		STC(std::bind(
			&ThreadLifetimeMgmtOp::resumeThreadReq1_posted,
			request.get(), request)));
}

// CPU management method implementations
int ComponentThread::getAvailableCpuCount()
{
	int cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpuCount <= 0)
	{
		throw std::runtime_error(std::string(__func__)
			+ ": Failed to determine CPU count");
	}

	// Check if std::thread::hardware_concurrency() matches sysconf result
	unsigned int hwConcurrency = std::thread::hardware_concurrency();
	if (hwConcurrency != static_cast<unsigned int>(cpuCount))
	{
		std::cerr << "Warning: CPU count mismatch - "
			"std::thread::hardware_concurrency() = "
			<< hwConcurrency << ", sysconf(_SC_NPROCESSORS_ONLN) = "
			<< cpuCount << "\n";
	}

	return cpuCount;
}

void PuppetThread::pinToCpu(int cpuId)
{
	if (cpuId < 0)
	{
		throw std::runtime_error(std::string(__func__)
			+ ": Invalid CPU ID: " + std::to_string(cpuId));
	}

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(cpuId, &cpuset);

	int result = pthread_setaffinity_np(
		thread.native_handle(), sizeof(cpu_set_t), &cpuset);
	if (result != 0)
	{
		throw std::runtime_error(std::string(__func__)
			+ ": Failed to pin thread to CPU " + std::to_string(cpuId)
			+ ": " + std::strerror(result));
	}

	pinnedCpuId = cpuId;
}

} // namespace sscl
