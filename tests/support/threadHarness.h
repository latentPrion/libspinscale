#ifndef SPINSCALE_TEST_SUPPORT_THREAD_HARNESS_H
#define SPINSCALE_TEST_SUPPORT_THREAD_HARNESS_H

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <spinscale/co/invokers.h>
#include <spinscale/co/postingPromise.h>
#include <spinscale/component.h>
#include <spinscale/componentThread.h>

namespace sscl::tests {

constexpr std::chrono::milliseconds defaultIdleTimeout{800};
constexpr std::chrono::milliseconds defaultTotalTimeout{10000};
constexpr std::chrono::milliseconds defaultPostingTaskTimeout{10000};

enum class PostingThreadRole : sscl::ThreadId
{
	CALLER = 70,
	CALLEE = 71,
	ALTERNATE = 72,
	BODY = 73,
	WORLD = 74,
	LEG = 75,
};

std::string threadRoleName(PostingThreadRole role);

class IoContextPump
{
public:
	static void pumpUntilIdle(
		boost::asio::io_context &ioContext,
		std::chrono::milliseconds idleTimeout = defaultIdleTimeout,
		std::chrono::milliseconds totalTimeout = defaultTotalTimeout);

	template <typename Predicate>
	static bool pumpUntil(
		boost::asio::io_context &ioContext,
		Predicate &&predicate,
		std::chrono::milliseconds idleTimeout = defaultIdleTimeout,
		std::chrono::milliseconds totalTimeout = defaultTotalTimeout)
	{
		const auto totalDeadline =
			std::chrono::steady_clock::now() + totalTimeout;
		auto lastProgress = std::chrono::steady_clock::now();

		while (std::chrono::steady_clock::now() < totalDeadline)
		{
			if (std::invoke(predicate)) {
				return true;
			}

			if (ioContext.poll_one() > 0)
			{
				lastProgress = std::chrono::steady_clock::now();
				continue;
			}

			if (std::chrono::steady_clock::now() - lastProgress >= idleTimeout) {
				return std::invoke(predicate);
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		return std::invoke(predicate);
	}
};

class ThreadBoundComponent final
:	public sscl::pptr::PuppeteerComponent
{
public:
	ThreadBoundComponent();
	void handleLoopExceptionHook() override;

	std::exception_ptr loopException;
};

class DedicatedIoThread
{
public:
	explicit DedicatedIoThread(PostingThreadRole role);
	~DedicatedIoThread();

	DedicatedIoThread(const DedicatedIoThread &) = delete;
	DedicatedIoThread &operator=(const DedicatedIoThread &) = delete;
	DedicatedIoThread(DedicatedIoThread &&) = delete;
	DedicatedIoThread &operator=(DedicatedIoThread &&) = delete;

	boost::asio::io_context &ioContext();
	sscl::ThreadId threadId() const noexcept;
	std::thread::id osThreadId() const;
	std::shared_ptr<sscl::PuppeteerThread> componentThread() const;

	void requestStop();
	void joinThread();
	void stopAndJoin();

	struct StartupState;

	template <typename Function>
	void post(Function &&function)
	{
		boost::asio::post(
			ioContext(),
			std::forward<Function>(function));
	}

	template <typename Function>
	auto runSync(Function &&function)
		-> std::invoke_result_t<Function &>
	{
		using Result = std::invoke_result_t<Function &>;

		if (std::this_thread::get_id() == osThreadId()) {
			if constexpr (std::is_void_v<Result>) {
				std::invoke(function);
				return;
			} else {
				return std::invoke(function);
			}
		}

		auto promise = std::make_shared<std::promise<Result>>();
		auto future = promise->get_future();

		post(
			[promise, function = std::forward<Function>(function)]() mutable
			{
				try
				{
					if constexpr (std::is_void_v<Result>)
					{
						std::invoke(function);
						promise->set_value();
					}
					else
					{
						promise->set_value(std::invoke(function));
					}
				}
				catch (...)
				{
					promise->set_exception(std::current_exception());
				}
			});

		return future.get();
	}

private:
	void releaseStartupBarrier();
	void waitUntilInitialized();

	PostingThreadRole role;
	std::shared_ptr<StartupState> startupState;
	ThreadBoundComponent component;
	std::shared_ptr<sscl::PuppeteerThread> thread;
};

class ThreadRegistry
{
public:
	static void registerThread(
		PostingThreadRole role,
		DedicatedIoThread &thread);
	static void unregisterThread(
		PostingThreadRole role,
		DedicatedIoThread &expectedThread);
	static boost::asio::io_context &ioContext(PostingThreadRole role);
	static std::thread::id osThreadId(PostingThreadRole role);

private:
	static std::mutex &registryMutex();
	static std::map<PostingThreadRole, DedicatedIoThread *> &threadsByRole();
};

template <PostingThreadRole role>
struct PostingThreadTag
{
	static boost::asio::io_context &io_context()
	{
		return ThreadRegistry::ioContext(role);
	}
};

template <PostingThreadRole role, typename T>
using RolePostingPromise =
	sscl::co::TaggedPostingPromise<T, PostingThreadTag<role>>;

template <PostingThreadRole role>
struct RolePostingPromiseTemplate
{
	template <typename T>
	using Type = RolePostingPromise<role, T>;
};

template <PostingThreadRole role, typename T>
using RoleViralPostingInvoker =
	sscl::co::ViralPostingInvoker<
		RolePostingPromiseTemplate<role>::template Type,
		T>;

template <PostingThreadRole role>
using RoleNonViralPostingInvoker =
	sscl::co::NonViralPostingInvoker<
		RolePostingPromiseTemplate<role>::template Type>;

class PostingThreadSet
{
public:
	PostingThreadSet();
	~PostingThreadSet();

	PostingThreadSet(const PostingThreadSet &) = delete;
	PostingThreadSet &operator=(const PostingThreadSet &) = delete;
	PostingThreadSet(PostingThreadSet &&) = delete;
	PostingThreadSet &operator=(PostingThreadSet &&) = delete;

	DedicatedIoThread &thread(PostingThreadRole role);
	DedicatedIoThread &caller();
	DedicatedIoThread &callee();
	DedicatedIoThread &alternate();
	DedicatedIoThread &body();
	DedicatedIoThread &world();
	DedicatedIoThread &leg();

private:
	void registerAllThreads();
	void unregisterAllThreads();
	void installCallerAsPuppeteer();
	void restorePreviousPuppeteer();
	void requestStopOnAllThreads();
	void joinAllThreads();

	DedicatedIoThread callerThread;
	DedicatedIoThread calleeThread;
	DedicatedIoThread alternateThread;
	DedicatedIoThread bodyThread;
	DedicatedIoThread worldThread;
	DedicatedIoThread legThread;
	std::shared_ptr<sscl::PuppeteerThread> previousPuppeteerThread;
	sscl::ThreadId previousPuppeteerThreadId = 0;
};

template <typename Function>
auto RunOnThread(DedicatedIoThread &thread, Function &&function)
	-> std::invoke_result_t<Function &>
{
	return thread.runSync(std::forward<Function>(function));
}

class CrossThreadTrace
{
public:
	void recordConstructionThread();
	void recordCalleeExecutionThread();
	void recordFinalSuspendThread();
	void recordAwaitResumeThread();
	void recordCompletionCallbackThread();

	std::thread::id constructionThread() const;
	std::thread::id calleeExecutionThread() const;
	std::thread::id finalSuspendThread() const;
	std::thread::id awaitResumeThread() const;
	std::thread::id completionCallbackThread() const;

private:
	void record(std::thread::id &slot);
	std::thread::id read(const std::thread::id &slot) const;

	mutable std::mutex mutex;
	std::thread::id constructionThreadId;
	std::thread::id calleeExecutionThreadId;
	std::thread::id finalSuspendThreadId;
	std::thread::id awaitResumeThreadId;
	std::thread::id completionCallbackThreadId;
};

template <typename InvokerFactory>
void runNonViralPostingTask(
	DedicatedIoThread &callerThread,
	InvokerFactory &&invokerFactory,
	std::chrono::milliseconds timeout = defaultPostingTaskTimeout)
{
	using Factory = std::decay_t<InvokerFactory>;
	using Invoker = std::invoke_result_t<
		Factory &, std::exception_ptr &, std::function<void()>>;

	struct TaskState
	{
		explicit TaskState(Factory factoryIn)
		: factory(std::move(factoryIn))
		{}

		Factory factory;
		std::exception_ptr coroutineException;
		std::exception_ptr taskException;
		std::optional<Invoker> invoker;
		std::mutex mutex;
		std::condition_variable condition;
		bool completed = false;
	};

	auto taskState = std::make_shared<TaskState>(
		std::forward<InvokerFactory>(invokerFactory));

	callerThread.post(
		[taskState]()
		{
			auto completeTask = [taskState]()
			{
				taskState->taskException = taskState->coroutineException;
				taskState->invoker.reset();

				{
					std::lock_guard<std::mutex> guard(taskState->mutex);
					taskState->completed = true;
				}

				taskState->condition.notify_one();
			};

			try
			{
				taskState->invoker.emplace(
					std::invoke(
						taskState->factory,
						taskState->coroutineException,
						std::move(completeTask)));
			}
			catch (...)
			{
				{
					std::lock_guard<std::mutex> guard(taskState->mutex);
					taskState->taskException = std::current_exception();
					taskState->completed = true;
				}

				taskState->condition.notify_one();
			}
		});

	std::unique_lock<std::mutex> lock(taskState->mutex);
	const bool completed = taskState->condition.wait_for(
		lock,
		timeout,
		[&taskState]() { return taskState->completed; });

	if (!completed) {
		throw std::runtime_error("Timed out waiting for posting coroutine task");
	}

	std::exception_ptr taskException = taskState->taskException;
	lock.unlock();

	if (taskException) {
		std::rethrow_exception(taskException);
	}
}

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_THREAD_HARNESS_H
