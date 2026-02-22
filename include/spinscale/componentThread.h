#ifndef COMPONENT_THREAD_H
#define COMPONENT_THREAD_H

#include <boostAsioLinkageFix.h>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <boost/asio/io_service.hpp>
#include <stdexcept>
#include <queue>
#include <functional>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <memory>
#include <spinscale/callback.h>
#include <cstdint>
#include <string>

namespace sscl {

class PuppetComponent;
class PuppeteerThread;
class PuppetThread;

namespace pptr {
class PuppeteerComponent;
}

// ThreadId is a generic type - application-specific enums should be defined elsewhere
typedef uint8_t ThreadId;

class ComponentThread
{
protected:
	ComponentThread(ThreadId _id, std::string _name)
	:	id(_id), name(std::move(_name)), work(io_service)
	{}

public:
	virtual ~ComponentThread() = default;

	void cleanup(void);

	boost::asio::io_service& getIoService(void) { return io_service; }

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
	boost::asio::io_service io_service;
	boost::asio::io_service::work work;
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
		preJoltHookFn preJoltFn)
	:	ComponentThread(_id, std::move(name)),
	pinnedCpuId(-1),
	pause_work(pause_io_service),
	entryFnArguments(*this, component, preJoltFn),
	thread(std::move(entryPoint), std::cref(entryFnArguments))
	{}

	virtual ~PuppetThread() = default;

	void initializeTls(void);

	// Thread management methods
	typedef std::function<void()> threadLifetimeMgmtOpCbFn;
	void startThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback);
	void exitThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback);
	void pauseThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback);
	void resumeThreadReq(Callback<threadLifetimeMgmtOpCbFn> callback);

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
	void joltThreadReq(
		const std::shared_ptr<PuppetThread>& selfPtr,
		Callback<threadLifetimeMgmtOpCbFn> callback);

	// CPU management methods
	void pinToCpu(int cpuId);

public:
	int pinnedCpuId;
	boost::asio::io_service pause_io_service;
	boost::asio::io_service::work pause_work;

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
