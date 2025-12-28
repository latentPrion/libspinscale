#ifndef ASYNCHRONOUS_BRIDGE_H
#define ASYNCHRONOUS_BRIDGE_H

#include <boostAsioLinkageFix.h>
#include <atomic>
#include <boost/asio/io_service.hpp>

namespace sscl {

class AsynchronousBridge
{
public:
	AsynchronousBridge(boost::asio::io_service &io_service)
	: isAsyncOperationComplete(false), io_service(io_service)
	{}

	void setAsyncOperationComplete(void)
	{
		/**		EXPLANATION:
		 * This empty post()ed message is necessary to ensure that the thread
		 * that's waiting on the io_service is signaled to wake up and check
		 * the io_service's queue.
		 */
		isAsyncOperationComplete.store(true);
		io_service.post([]{});
	}

	void waitForAsyncOperationCompleteOrIoServiceStopped(void)
	{
		for (;;)
		{
			io_service.run_one();
			if (isAsyncOperationComplete.load() || io_service.stopped())
				{ break; }

			/**	EXPLANATION:
			 * In the mrntt and mind thread loops we call checkException() after
			 * run() returns, but we don't have to do that here because
			 * setException() calls stop.
			 *
			 * So if an exception is set on our thread, we'll break out of this
			 * loop due to the check for stopped() above, and that'll take us
			 * back out to the main loop, where we'll catch the exception.
			 */
		}
	}

	bool exitedBecauseIoServiceStopped(void) const
		{ return io_service.stopped(); }

private:
	std::atomic<bool> isAsyncOperationComplete;
	boost::asio::io_service &io_service;
};

} // namespace sscl

#endif // ASYNCHRONOUS_BRIDGE_H
