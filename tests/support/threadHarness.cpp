#include <support/threadHarness.h>

#include <cstdlib>
#include <iostream>

namespace sscl::tests {

struct DedicatedIoThread::StartupState
{
	std::mutex mutex;
	std::condition_variable condition;
	std::thread::id osThreadId;
	std::exception_ptr startupException;
	bool allowInitialization = false;
	bool initialized = false;
};

namespace {

constexpr const char *callerThreadName = "test:caller";
constexpr const char *calleeThreadName = "test:callee";
constexpr const char *alternateThreadName = "test:alternate";
constexpr const char *bodyThreadName = "test:body";
constexpr const char *worldThreadName = "test:world";
constexpr const char *legThreadName = "test:leg";

void runDedicatedThread(
	const std::shared_ptr<DedicatedIoThread::StartupState> &state,
	const sscl::PuppeteerThread::EntryFnArguments &args)
{
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->condition.wait(
			lock,
			[&state]() { return state->allowInitialization; });
	}

	try
	{
		args.usableBeforeJolt.initializeTls();

		{
			std::lock_guard<std::mutex> guard(state->mutex);
			state->osThreadId = std::this_thread::get_id();
			state->initialized = true;
		}

		state->condition.notify_all();

		args.usableBeforeJolt.getIoContext().restart();
		args.usableBeforeJolt.getIoContext().run();
	}
	catch (...)
	{
		{
			std::lock_guard<std::mutex> guard(state->mutex);
			state->startupException = std::current_exception();
			state->initialized = true;
		}

		state->condition.notify_all();
	}
}

} // namespace

std::string threadRoleName(PostingThreadRole role)
{
	switch (role)
	{
	case PostingThreadRole::CALLER:
		return callerThreadName;
	case PostingThreadRole::CALLEE:
		return calleeThreadName;
	case PostingThreadRole::ALTERNATE:
		return alternateThreadName;
	case PostingThreadRole::BODY:
		return bodyThreadName;
	case PostingThreadRole::WORLD:
		return worldThreadName;
	case PostingThreadRole::LEG:
		return legThreadName;
	}

	throw std::runtime_error("Unknown PostingThreadRole");
}

void IoContextPump::pumpUntilIdle(
	boost::asio::io_context &ioContext,
	std::chrono::milliseconds idleTimeout,
	std::chrono::milliseconds totalTimeout)
{
	const auto totalDeadline =
		std::chrono::steady_clock::now() + totalTimeout;
	auto lastProgress = std::chrono::steady_clock::now();

	while (std::chrono::steady_clock::now() < totalDeadline)
	{
		if (ioContext.poll_one() > 0)
		{
			lastProgress = std::chrono::steady_clock::now();
			continue;
		}

		if (std::chrono::steady_clock::now() - lastProgress >= idleTimeout) {
			return;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

ThreadBoundComponent::ThreadBoundComponent()
:	sscl::pptr::PuppeteerComponent(nullptr)
{
}

void ThreadBoundComponent::handleLoopExceptionHook()
{
	loopException = std::current_exception();
}

DedicatedIoThread::DedicatedIoThread(PostingThreadRole roleIn)
:	role(roleIn),
	startupState(std::make_shared<StartupState>()),
	component(),
	thread(std::make_shared<sscl::PuppeteerThread>(
		static_cast<sscl::ThreadId>(roleIn),
		threadRoleName(roleIn),
		[state = startupState](
			const sscl::PuppeteerThread::EntryFnArguments &args)
		{
			runDedicatedThread(state, args);
		},
		component,
		nullptr))
{
	component.thread = thread;
	releaseStartupBarrier();
	waitUntilInitialized();
}

DedicatedIoThread::~DedicatedIoThread()
{
	stopAndJoin();
}

boost::asio::io_context &DedicatedIoThread::ioContext()
{
	return thread->getIoContext();
}

sscl::ThreadId DedicatedIoThread::threadId() const noexcept
{
	return static_cast<sscl::ThreadId>(role);
}

std::thread::id DedicatedIoThread::osThreadId() const
{
	std::lock_guard<std::mutex> guard(startupState->mutex);
	return startupState->osThreadId;
}

std::shared_ptr<sscl::PuppeteerThread> DedicatedIoThread::componentThread() const
{
	return thread;
}

void DedicatedIoThread::stopAndJoin()
{
	if (!thread) {
		return;
	}

	releaseStartupBarrier();
	thread->getIoContext().stop();

	if (thread->thread.joinable()) {
		thread->thread.join();
	}

	thread.reset();
}

void DedicatedIoThread::releaseStartupBarrier()
{
	{
		std::lock_guard<std::mutex> guard(startupState->mutex);
		startupState->allowInitialization = true;
	}

	startupState->condition.notify_all();
}

void DedicatedIoThread::waitUntilInitialized()
{
	std::unique_lock<std::mutex> lock(startupState->mutex);
	const bool initialized = startupState->condition.wait_for(
		lock,
		defaultPostingTaskTimeout,
		[this]() { return startupState->initialized; });

	if (!initialized) {
		throw std::runtime_error("Timed out waiting for test thread startup");
	}

	std::exception_ptr startupException = startupState->startupException;
	lock.unlock();

	if (startupException) {
		std::rethrow_exception(startupException);
	}
}

void ThreadRegistry::registerThread(
	PostingThreadRole role,
	DedicatedIoThread &thread)
{
	std::lock_guard<std::mutex> guard(registryMutex());
	auto [iterator, inserted] = threadsByRole().emplace(role, &thread);

	if (!inserted) {
		throw std::runtime_error(
			"Test thread role already registered for " + threadRoleName(role));
	}
}

void ThreadRegistry::unregisterThread(
	PostingThreadRole role,
	DedicatedIoThread &expectedThread)
{
	std::lock_guard<std::mutex> guard(registryMutex());
	auto iterator = threadsByRole().find(role);

	if (iterator == threadsByRole().end()) {
		return;
	}

	if (iterator->second != &expectedThread) {
		throw std::runtime_error(
			"Test thread role registered to a different thread for "
			+ threadRoleName(role));
	}

	threadsByRole().erase(iterator);
}

boost::asio::io_context &ThreadRegistry::ioContext(PostingThreadRole role)
{
	std::lock_guard<std::mutex> guard(registryMutex());
	auto iterator = threadsByRole().find(role);

	if (iterator == threadsByRole().end()) {
		throw std::runtime_error(
			"No test thread registered for " + threadRoleName(role));
	}

	return iterator->second->ioContext();
}

std::thread::id ThreadRegistry::osThreadId(PostingThreadRole role)
{
	std::lock_guard<std::mutex> guard(registryMutex());
	auto iterator = threadsByRole().find(role);

	if (iterator == threadsByRole().end()) {
		throw std::runtime_error(
			"No test thread registered for " + threadRoleName(role));
	}

	return iterator->second->osThreadId();
}

std::mutex &ThreadRegistry::registryMutex()
{
	static std::mutex mutex;
	return mutex;
}

std::map<PostingThreadRole, DedicatedIoThread *> &
ThreadRegistry::threadsByRole()
{
	static std::map<PostingThreadRole, DedicatedIoThread *> threads;
	return threads;
}

PostingThreadSet::PostingThreadSet()
:	callerThread(PostingThreadRole::CALLER),
	calleeThread(PostingThreadRole::CALLEE),
	alternateThread(PostingThreadRole::ALTERNATE),
	bodyThread(PostingThreadRole::BODY),
	worldThread(PostingThreadRole::WORLD),
	legThread(PostingThreadRole::LEG)
{
	previousPuppeteerThread = sscl::ComponentThread::getPptr();
	previousPuppeteerThreadId = sscl::pptr::puppeteerThreadId;
	registerAllThreads();
	installCallerAsPuppeteer();
}

PostingThreadSet::~PostingThreadSet()
{
	restorePreviousPuppeteer();
	unregisterAllThreads();
}

void PostingThreadSet::registerAllThreads()
{
	ThreadRegistry::registerThread(PostingThreadRole::CALLER, callerThread);
	ThreadRegistry::registerThread(PostingThreadRole::CALLEE, calleeThread);
	ThreadRegistry::registerThread(PostingThreadRole::ALTERNATE, alternateThread);
	ThreadRegistry::registerThread(PostingThreadRole::BODY, bodyThread);
	ThreadRegistry::registerThread(PostingThreadRole::WORLD, worldThread);
	ThreadRegistry::registerThread(PostingThreadRole::LEG, legThread);
}

void PostingThreadSet::unregisterAllThreads()
{
	ThreadRegistry::unregisterThread(PostingThreadRole::CALLER, callerThread);
	ThreadRegistry::unregisterThread(PostingThreadRole::CALLEE, calleeThread);
	ThreadRegistry::unregisterThread(
		PostingThreadRole::ALTERNATE,
		alternateThread);
	ThreadRegistry::unregisterThread(PostingThreadRole::BODY, bodyThread);
	ThreadRegistry::unregisterThread(PostingThreadRole::WORLD, worldThread);
	ThreadRegistry::unregisterThread(PostingThreadRole::LEG, legThread);
}

void PostingThreadSet::installCallerAsPuppeteer()
{
	sscl::ComponentThread::setPuppeteerThreadId(
		static_cast<sscl::ThreadId>(PostingThreadRole::CALLER));
	sscl::ComponentThread::setPuppeteerThread(callerThread.componentThread());
}

void PostingThreadSet::restorePreviousPuppeteer()
{
	sscl::ComponentThread::setPuppeteerThreadId(previousPuppeteerThreadId);
	sscl::ComponentThread::setPuppeteerThread(previousPuppeteerThread);
}

DedicatedIoThread &PostingThreadSet::thread(PostingThreadRole role)
{
	switch (role)
	{
	case PostingThreadRole::CALLER:
		return callerThread;
	case PostingThreadRole::CALLEE:
		return calleeThread;
	case PostingThreadRole::ALTERNATE:
		return alternateThread;
	case PostingThreadRole::BODY:
		return bodyThread;
	case PostingThreadRole::WORLD:
		return worldThread;
	case PostingThreadRole::LEG:
		return legThread;
	}

	throw std::runtime_error("Unknown PostingThreadRole");
}

DedicatedIoThread &PostingThreadSet::caller()
{
	return callerThread;
}

DedicatedIoThread &PostingThreadSet::callee()
{
	return calleeThread;
}

DedicatedIoThread &PostingThreadSet::alternate()
{
	return alternateThread;
}

DedicatedIoThread &PostingThreadSet::body()
{
	return bodyThread;
}

DedicatedIoThread &PostingThreadSet::world()
{
	return worldThread;
}

DedicatedIoThread &PostingThreadSet::leg()
{
	return legThread;
}

void CrossThreadTrace::recordConstructionThread()
{
	record(constructionThreadId);
}

void CrossThreadTrace::recordCalleeExecutionThread()
{
	record(calleeExecutionThreadId);
}

void CrossThreadTrace::recordFinalSuspendThread()
{
	record(finalSuspendThreadId);
}

void CrossThreadTrace::recordAwaitResumeThread()
{
	record(awaitResumeThreadId);
}

void CrossThreadTrace::recordCompletionCallbackThread()
{
	record(completionCallbackThreadId);
}

std::thread::id CrossThreadTrace::constructionThread() const
{
	return read(constructionThreadId);
}

std::thread::id CrossThreadTrace::calleeExecutionThread() const
{
	return read(calleeExecutionThreadId);
}

std::thread::id CrossThreadTrace::finalSuspendThread() const
{
	return read(finalSuspendThreadId);
}

std::thread::id CrossThreadTrace::awaitResumeThread() const
{
	return read(awaitResumeThreadId);
}

std::thread::id CrossThreadTrace::completionCallbackThread() const
{
	return read(completionCallbackThreadId);
}

void CrossThreadTrace::record(std::thread::id &slot)
{
	std::lock_guard<std::mutex> guard(mutex);
	slot = std::this_thread::get_id();
}

std::thread::id CrossThreadTrace::read(const std::thread::id &slot) const
{
	std::lock_guard<std::mutex> guard(mutex);
	return slot;
}

} // namespace sscl::tests
