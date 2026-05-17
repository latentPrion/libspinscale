#ifndef CO_QUTEX_H
#define CO_QUTEX_H

#include <config.h>
#include <cassert>
#include <coroutine>
#include <deque>
#include <stdexcept>
#include <type_traits>

#ifdef CONFIG_LIBSSCL_DEBUG_CO
#include <iostream>
#include <thread>
#endif

#include <boost/asio/io_service.hpp>
#include <boost/asio/post.hpp>

#include <spinscale/componentThread.h>
#include <spinscale/co/promiseChainWalker.h>
#include <spinscale/spinLock.h>

namespace sscl::co {

class CoQutex
{
public:
	class ReleaseHandle;

	CoQutex() noexcept = default;
	CoQutex(const CoQutex &) = delete;
	CoQutex(CoQutex &&) noexcept = delete;
	CoQutex &operator=(const CoQutex &) = delete;
	CoQutex &operator=(CoQutex &&) noexcept = delete;
	~CoQutex() = default;

	struct AcquireInvocationAndSuspensionPolicy
	{
		AcquireInvocationAndSuspensionPolicy(CoQutex &_coQutex) noexcept
		: coQutex(_coQutex)
		{}

		~AcquireInvocationAndSuspensionPolicy() noexcept = default;

		struct WaitingCoroutine
		{
			WaitingCoroutine(
				std::coroutine_handle<void> _callerSchedHandle,
				boost::asio::io_service &_callerIoContext,
				PromiseChainLink &_waitingPromise) noexcept
			: callerSchedHandle(_callerSchedHandle),
			callerIoContext(_callerIoContext),
			waitingPromise(_waitingPromise)
			{}

			std::coroutine_handle<void> callerSchedHandle;
			boost::asio::io_service &callerIoContext;
			PromiseChainLink &waitingPromise;
		};

		bool await_ready() noexcept { return false; }

		template <typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> callerSchedHandle)
		{
			static_assert(
				std::is_base_of_v<PromiseChainLink, Promise>,
				"CoQutex acquire requires a promise type derived from PromiseChainLink");

			acquirerChainLink = &callerSchedHandle.promise();

			walkCallerPromiseChainFrom(
				static_cast<const PromiseChainLink &>(callerSchedHandle.promise()),
				[this](const PromiseChainLink &link)
				{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
					std::cout << __func__ << ": " << std::this_thread::get_id() << " Walking caller promise chain.\n";
#endif
					if (link.holdsAcquiredLock(coQutex)) {
						throw std::runtime_error("Deadlock detected: CoQutex re-acquire on caller promise chain.");
					}
				});

			sscl::SpinLock::Guard guard(coQutex.spinLock);
			if (!coQutex.isOwned) {
				coQutex.isOwned = true;
				return false;
			}
			coQutex.waitingCoroutines.emplace_back(
				std::coroutine_handle<void>::from_address(callerSchedHandle.address()),
				sscl::ComponentThread::getSelf()->getIoService(),
				*acquirerChainLink);
			return true;
		}

		ReleaseHandle
		// [[nodiscard("store co_await result; lock is held until ReleaseHandle is released")]]
		await_resume() noexcept;

		CoQutex &coQutex;

	private:
		PromiseChainLink *acquirerChainLink = nullptr;
	};

	AcquireInvocationAndSuspensionPolicy
	// [[nodiscard("store co_await result; lock is held until ReleaseHandle is released")]]
	getAcquireInvocationAndSuspensionPolicy() noexcept
	{
		return AcquireInvocationAndSuspensionPolicy(*this);
	}

private:
	friend class ReleaseHandle;

	void release() noexcept
	{
		sscl::SpinLock::Guard guard(spinLock);

		assert(isOwned);
		if (waitingCoroutines.empty()) {
			isOwned = false;
			return;
		}

		auto &frontWaitingCoroutine = waitingCoroutines.front();
		boost::asio::post(
			frontWaitingCoroutine.callerIoContext,
			frontWaitingCoroutine.callerSchedHandle);
		waitingCoroutines.pop_front();
	}

	sscl::SpinLock spinLock;
	bool isOwned = false;
	std::deque<AcquireInvocationAndSuspensionPolicy::WaitingCoroutine> waitingCoroutines;
};

//[[nodiscard("store co_await result; lock is held until ReleaseHandle is released")]]
class CoQutex::ReleaseHandle
{
public:
	ReleaseHandle(PromiseChainLink &promiseChainLinkIn, CoQutex &coQutexIn) noexcept
	: promiseChainLink(promiseChainLinkIn),
	coQutex(coQutexIn)
	{}

	ReleaseHandle(const ReleaseHandle &) = delete;
	ReleaseHandle &operator=(const ReleaseHandle &) = delete;

	ReleaseHandle(ReleaseHandle &&other) noexcept
	: promiseChainLink(other.promiseChainLink),
	coQutex(other.coQutex),
	armed(other.armed)
	{
		other.armed = false;
	}

	ReleaseHandle &operator=(ReleaseHandle &&other) noexcept = delete;

	~ReleaseHandle() noexcept
	{
		if (armed)
			{ release(); }
	}

	void release() noexcept
	{
		if (!armed)
			{ return; }

		armed = false;
		promiseChainLink.removeAcquiredLock(coQutex);
		coQutex.release();
	}

	void operator()() noexcept
	{
		release();
	}

private:
	PromiseChainLink &promiseChainLink;
	CoQutex &coQutex;
	bool armed = true;
};

inline CoQutex::ReleaseHandle
// [[nodiscard("store co_await result; lock is held until ReleaseHandle is released")]]
CoQutex::AcquireInvocationAndSuspensionPolicy::await_resume() noexcept
{
	assert(acquirerChainLink != nullptr);
	acquirerChainLink->addAcquiredLock(coQutex);
	return CoQutex::ReleaseHandle(*acquirerChainLink, coQutex);
}

} // namespace sscl::co

#endif // CO_QUTEX_H
