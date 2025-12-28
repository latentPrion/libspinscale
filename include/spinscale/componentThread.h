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

class MarionetteThread;
class PuppetThread;

// ThreadId is a generic type - application-specific enums should be defined elsewhere
typedef uint8_t ThreadId;

class ComponentThread
{
protected:
	ComponentThread(ThreadId _id)
    :   id(_id), name(getThreadName(_id)),
        work(io_service)
	{}

public:
	virtual ~ComponentThread() = default;

	// getThreadName implementation is provided by application code
	static std::string getThreadName(ThreadId id);

	void cleanup(void);

	boost::asio::io_service& getIoService(void) { return io_service; }

	static const std::shared_ptr<ComponentThread> getSelf(void);
	static bool tlsInitialized(void);
	static std::shared_ptr<MarionetteThread> getMrntt();

	typedef void (mainFn)(ComponentThread &self);

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

class MarionetteThread
:	public std::enable_shared_from_this<MarionetteThread>,
	public ComponentThread
{
public:
	MarionetteThread(ThreadId id = 0)
	:	ComponentThread(id),
	thread(main, std::ref(*this))
	{
	}

	static void main(MarionetteThread& self);
	void initializeTls(void);

public:
	std::thread thread;
};

class PuppetThread
:	public std::enable_shared_from_this<PuppetThread>,
	public ComponentThread
{
public:
	enum class ThreadOp
	{
		START,
		PAUSE,
		RESUME,
		EXIT,
		JOLT,
		N_ITEMS
	};

	PuppetThread(ThreadId _id)
	:   ComponentThread(_id),
	pinnedCpuId(-1),
	pause_work(pause_io_service),
	thread(main, std::ref(*this))
	{
	}

	virtual ~PuppetThread() = default;

	static void main(PuppetThread& self);
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

protected:
	/**
	 * Handle exception - called from main() when an exception occurs.
	 * Derived classes can override to provide application-specific handling.
	 */
	virtual void handleException() {}

public:
	int pinnedCpuId;
	boost::asio::io_service pause_io_service;
	boost::asio::io_service::work pause_work;
	std::thread thread;

public:
	class ThreadLifetimeMgmtOp;
};

namespace mrntt {
extern std::shared_ptr<MarionetteThread> thread;

// Forward declaration for marionette thread ID management
// Must be after sscl namespace so ThreadId is defined
extern ThreadId marionetteThreadId;
void setMarionetteThreadId(ThreadId id);
} // namespace mrntt
}

#endif // COMPONENT_THREAD_H
