#ifndef ASYNCHRONOUS_BRIDGE_H
#define ASYNCHRONOUS_BRIDGE_H

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

namespace sscl::cps {

class AsynchronousBridge
{
public:
	AsynchronousBridge(boost::asio::io_context &io_context)
	: isAsyncOperationComplete(false), io_context(io_context)
	{}

	void setAsyncOperationComplete(void)
	{
		/**		EXPLANATION:
		 * This empty post()ed message is necessary to ensure that the thread
		 * that's waiting on the io_context is signaled to wake up and check
		 * the io_context's queue.
		 */
		isAsyncOperationComplete.store(true);
		boost::asio::post(io_context, []{});
	}

	void waitForAsyncOperationCompleteOrIoContextStopped(void)
	{
		for (;;)
		{
			io_context.run_one();
			if (isAsyncOperationComplete.load() || io_context.stopped())
				{ break; }

			/**	EXPLANATION:
			 * In the puppeteer and mind thread loops we call checkException()
			 * after run() returns, but we don't have to do that here because
			 * setException() calls stop().
			 *
			 * So if an exception is set on our thread, we'll break out of this
			 * loop due to the check for stopped() above, and that'll take us
			 * back out to the main loop, where we'll catch the exception.
			 */
		}
	}

	bool exitedBecauseIoContextStopped(void) const
		{ return io_context.stopped(); }

private:
	std::atomic<bool> isAsyncOperationComplete;
	boost::asio::io_context &io_context;
};

} // namespace sscl::cps

#endif // ASYNCHRONOUS_BRIDGE_H
