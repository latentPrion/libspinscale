#ifndef CO_YIELD_H
#define CO_YIELD_H

#include <boostAsioLinkageFix.h>

#include <coroutine>

#include <boost/asio/post.hpp>

#include <spinscale/componentThread.h>

namespace sscl::co {

/**	EXPLANATION:
 * Cooperative yield onto the current ComponentThread io_context. Always
 * suspends, then posts the continuation so other queued handlers on this
 * thread can run before resume. Not a coroutine-entry Invoker; it does not
 * own a callee frame.
 */
struct YieldInvoker
{
	bool await_ready() const noexcept { return false; }

	void await_suspend(std::coroutine_handle<> callerSchedHandle) noexcept
	{
		boost::asio::post(
			sscl::ComponentThread::getSelf()->getIoContext(),
			callerSchedHandle);
	}

	void await_resume() const noexcept {}
};

inline YieldInvoker getYieldInvoker() noexcept { return YieldInvoker{}; }

} // namespace sscl::co

#endif // CO_YIELD_H
