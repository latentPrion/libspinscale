#ifndef COMPONENT_THREAD_H
#define COMPONENT_THREAD_H

#include <atomic>
#include <thread>
#include <unordered_map>
#include <stdexcept>
#include <queue>
#include <functional>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <memory>
#include <coroutine>
#include <cstdint>
#include <string>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <spinscale/cps/callback.h>

namespace sscl {

class PuppetComponent;
class PuppeteerThread;
class PuppetThread;

namespace co {
class CoConditionVariable;
}

namespace pptr {
class PuppeteerComponent;
}

// ThreadId is a generic type - application-specific enums should be defined elsewhere
typedef uint8_t ThreadId;

class ComponentThread
{
protected:
	ComponentThread(ThreadId _id, std::string _name)
	: id(_id), name(std::move(_name)),
	io_context(),
	work(boost::asio::make_work_guard(io_context)),
	keepLooping(true)
	{}

public:
	virtual ~ComponentThread() = default;

	void cleanup(void);

	boost::asio::io_context& getIoContext(void) { return io_context; }

	static const std::shared_ptr<ComponentThread> getSelf(void);
	static bool tlsInitialized(void);
	static void setPuppeteerThread(const std::shared_ptr<PuppeteerThread> &t);
	static void setPuppeteerThreadId(ThreadId id);
	static std::shared_ptr<PuppeteerThread> getPptr();
	static std::shared_ptr<PuppeteerThread> getPuppeteer()
		{ return getPptr(); }

	// CPU management methods
	static int getAvailableCpuCount();

	typedef std::function<void()> mindShutdownIndOpCbFn;
	// Intentionally doesn't take a callback.
	void exceptionInd(const std::shared_ptr<ComponentThread> &faultyThread);
	// Intentionally doesn't take a callback.
	void userShutdownInd();

public:
	ThreadId id;
	std::string name;
	boost::asio::io_context io_context;
	boost::asio::executor_work_guard<
		boost::asio::io_context::executor_type> work;
	std::atomic<bool> keepLooping;
};

class PuppeteerThread
:	public std::enable_shared_from_this<PuppeteerThread>,
	public ComponentThread
{
public:
	typedef void (*preJoltHookFn)(PuppeteerThread &);

	struct EntryFnArguments
	{
		PuppeteerThread &usableBeforeJolt;
		/**	EXPLANATION:
		 * The `Puppet*Component` ref points at the Component object which this
		 * thread is associated with. However, we have no guarantee that this
		 * object has been constructed at the point of OS thread entry.
		 *
		 * Hence this ref must be dereferenced only after JOLT.
		 */
		pptr::PuppeteerComponent &useOnlyAfterJolt;
		preJoltHookFn preJoltHook;
	};

	using entryPointFn = std::function<void(const EntryFnArguments &)>;

	PuppeteerThread(
		ThreadId id, std::string name,
		entryPointFn entryPoint,
		pptr::PuppeteerComponent &component,
		preJoltHookFn preJoltFn)
	:	ComponentThread(id, std::move(name)),
	entryFnArguments(*this, component, preJoltFn),
	thread(std::move(entryPoint), std::cref(entryFnArguments))
	{}

	void initializeTls(void);
	void exitLoop(void);

public:
	EntryFnArguments entryFnArguments;
	/**	EXPLANATION:
	 * Must always be memberwise-initialized last.
	 * This ensures that the ref to this `ComponentThread` object, which is
	 * passed to the entry point function, is fully constructed when the OS
	 * thread begins executing.
	 */
	std::thread thread;
};

class PuppetThread
:	public std::enable_shared_from_this<PuppetThread>,
	public ComponentThread
{
public:
	typedef void (*preJoltHookFn)(PuppetThread &);

	struct EntryFnArguments
	{
		PuppetThread &usableBeforeJolt;
		/** See comment above in:
		 * PuppeteerThread::EntryFnArguments::useOnlyAfterJolt.
		 */
		PuppetComponent &useOnlyAfterJolt;
		preJoltHookFn preJoltHook;
	};

	using entryPointFn = std::function<void(const EntryFnArguments &)>;

	enum class ThreadOp
	{
		START,
		PAUSE,
		RESUME,
		EXIT,
		JOLT,
		N_ITEMS
	};

	PuppetThread(
		ThreadId _id, std::string name,
		entryPointFn entryPoint, PuppetComponent &component,
		preJoltHookFn preJoltFn);
	/** Out-of-line: unique_ptr<CoConditionVariable> needs a complete type. */
	virtual ~PuppetThread();

	void initializeTls(void);

	typedef std::function<void()> threadLifetimeMgmtOpCbFn;

	struct ViralThreadLifetimeMgmtInvoker
	{
		struct AsyncState
		{
			std::atomic<bool> settled{false};
			std::coroutine_handle<> callerSchedHandle;
		};

		ViralThreadLifetimeMgmtInvoker(
			ThreadOp _threadOp,
			PuppetThread &_parentThread,
			const std::shared_ptr<PuppetThread> &_selfPtr = nullptr)
		: threadOp(_threadOp),
		asyncState(std::make_shared<AsyncState>()),
		parentThread(_parentThread),
		selfPtr(_selfPtr),
		lifetimeMgmtCallback{
			nullptr,
			[asyncState = asyncState]()
			{
				asyncState->settled.store(true, std::memory_order_release);

				std::coroutine_handle<> handle =
					asyncState->callerSchedHandle;

				if (!handle) {
					return;
				}

				/**	Post resume to the puppeteer queue: direct resume() from
				 *	within an asio completion handler can destroy adapter
				 *	coroutine state while the handler is still unwinding.
				 */
				boost::asio::post(
					ComponentThread::getPptr()->getIoContext(),
					[handle]() { handle.resume(); });
			}}
		{
			if (threadOp == ThreadOp::JOLT && selfPtr == nullptr)
			{
				throw std::runtime_error(std::string(__func__)
					+ ": JOLT request must be made with a valid selfPtr");
			}

			switch (threadOp)
			{
			case ThreadOp::START:
				parentThread.startThreadReq(lifetimeMgmtCallback);
				break;
			case ThreadOp::PAUSE:
				parentThread.pauseThreadReq(lifetimeMgmtCallback);
				break;
			case ThreadOp::RESUME:
				parentThread.resumeThreadReq(lifetimeMgmtCallback);
				break;
			case ThreadOp::EXIT:
				parentThread.exitThreadReq(lifetimeMgmtCallback);
				break;
			case ThreadOp::JOLT:
				parentThread.joltThreadReq(selfPtr, lifetimeMgmtCallback);
				break;

			default:
				throw std::runtime_error(std::string(__func__)
					+ ": Invalid thread operation");
			}
		}

		bool await_ready() const noexcept
		{
			return asyncState->settled.load(std::memory_order_acquire);
		}

		bool await_suspend(
			std::coroutine_handle<> _callerSchedHandle) noexcept
		{
			if (asyncState->settled.load(std::memory_order_acquire)) {
				return false;
			}

			asyncState->callerSchedHandle = _callerSchedHandle;
			return true;
		}

		void await_resume() noexcept {}

		ThreadOp threadOp;
		std::shared_ptr<AsyncState> asyncState;
		PuppetThread &parentThread;
		const std::shared_ptr<PuppetThread> selfPtr;
		cps::Callback<threadLifetimeMgmtOpCbFn> lifetimeMgmtCallback;
	};

	// Thread lifetime management request invokers
	ViralThreadLifetimeMgmtInvoker startThreadAReq()
		{ return ViralThreadLifetimeMgmtInvoker(ThreadOp::START, *this); }
	ViralThreadLifetimeMgmtInvoker pauseThreadAReq()
		{ return ViralThreadLifetimeMgmtInvoker(ThreadOp::PAUSE, *this); }
	ViralThreadLifetimeMgmtInvoker resumeThreadAReq()
		{ return ViralThreadLifetimeMgmtInvoker(ThreadOp::RESUME, *this); }
	ViralThreadLifetimeMgmtInvoker exitThreadAReq()
		{ return ViralThreadLifetimeMgmtInvoker(ThreadOp::EXIT, *this); }

	void startThreadReq(cps::Callback<threadLifetimeMgmtOpCbFn> callback);
	void exitThreadReq(cps::Callback<threadLifetimeMgmtOpCbFn> callback);
	void pauseThreadReq(cps::Callback<threadLifetimeMgmtOpCbFn> callback);
	void resumeThreadReq(cps::Callback<threadLifetimeMgmtOpCbFn> callback);

	/**
	 * JOLTs this thread to begin processing after global initialization.
	 *
	 * JOLTing is the mechanism that allows threads to enter their main
	 * event loops and set up TLS vars after all global constructors have
	 * completed. This prevents race conditions during system startup.
	 *
	 * @param selfPtr Shared pointer to this thread (required because TLS
	 *                isn't set up yet, so shared_from_this() can't be used)
	 * @param callback Callback to invoke when JOLT completes
	 */
	ViralThreadLifetimeMgmtInvoker joltThreadAReq(
		const std::shared_ptr<PuppetThread> &selfPtr)
		{ return ViralThreadLifetimeMgmtInvoker(ThreadOp::JOLT, *this, selfPtr); }

	void joltThreadReq(
		const std::shared_ptr<PuppetThread>& selfPtr,
		cps::Callback<threadLifetimeMgmtOpCbFn> callback);

	// CPU management methods
	void pinToCpu(int cpuId);

	/** After main loops return: drop work guards and destroy both queues. */
	void destroyIoContextsAfterMainLoop(void);
	/** Wait until exitThreadReq has queued both dual-posts (then destroy). */
	void syncAwaitExitDualPostsCompleted(void);
	boost::asio::io_context& getPauseIoContext(void)
		{ return pause_io_context; }

public:
	int pinnedCpuId;
	boost::asio::io_context pause_io_context;
	boost::asio::executor_work_guard<
		boost::asio::io_context::executor_type> pause_work;
	/**	EXPLANATION:
	 * exitThreadReq dual-posts to main then pause, then signals this CV.
	 * defaultPuppetMain syncAwaits it before destroyIoContextsAfterMainLoop so
	 * the pause post cannot race destroyed / placement-new'd contexts.
	 * unique_ptr (not by-value): coConditionVariable.h includes this header, so
	 * the CV type is incomplete here; PuppetThread ctor/dtor live in the .cpp.
	 */
	std::unique_ptr<sscl::co::CoConditionVariable> exitDualPostsCompletedCv;

public:
	EntryFnArguments entryFnArguments;
	/** Must always be memberwise-initialized last.
	 * See comment on `PuppeteerThread::thread` for explanation.
	 */
	std::thread thread;

public:
	class ThreadLifetimeMgmtOp;
};

namespace pptr {
extern std::shared_ptr<PuppeteerThread> thread;
extern ThreadId puppeteerThreadId;
} // namespace pptr

} // namespace sscl

#endif // COMPONENT_THREAD_H
