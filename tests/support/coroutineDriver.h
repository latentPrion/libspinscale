#ifndef SPINSCALE_TEST_SUPPORT_COROUTINE_DRIVER_H
#define SPINSCALE_TEST_SUPPORT_COROUTINE_DRIVER_H

#include <exception>

#include <boost/asio/io_context.hpp>

#include <support/threadHarness.h>

namespace sscl::tests {

class CoroutineDriver
{
public:
	template <typename Invoker>
	static auto completedReturnValue(Invoker &invoker)
	{
		if (invoker.completedReturnValues().myExceptionPtr) {
			std::rethrow_exception(
				invoker.completedReturnValues().myExceptionPtr);
		}

		return invoker.completedReturnValues().myReturnValue;
	}

	template <typename Invoker>
	static auto pumpUntilIdleAndReturnValue(
		boost::asio::io_context &ioContext,
		Invoker &invoker)
	{
		IoContextPump::pumpUntilIdle(ioContext);
		return completedReturnValue(invoker);
	}
};

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_COROUTINE_DRIVER_H
