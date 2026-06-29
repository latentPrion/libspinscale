#ifndef SYNC_AWAIT_NON_VIRAL_CORO_H
#define SYNC_AWAIT_NON_VIRAL_CORO_H

#include <boostAsioLinkageFix.h>

#include <spinscale/componentThread.h>
#include <spinscale/co/nonViralTaskNursery.h>
#include <exception>
#include <functional>

namespace sscl::co {

/** Launch a non-viral coroutine on the current ComponentThread io_context and
 * block until it settles, rethrowing any stored exception.
 */
template<typename InvokerFactory>
void syncAwaitNonViralCoro(InvokerFactory&& _invokerFactory)
{
	std::exception_ptr slotException;

	NonViralTaskNursery nursery;
	nursery.openAdmission();
	nursery.launch(
		[&_invokerFactory](NonViralTaskNursery::Slot::Lease& lease)
		{
			return _invokerFactory(lease);
		},
		[&slotException](std::exception_ptr& exceptionPtr)
		{
			slotException = exceptionPtr;
		});
	nursery.closeAdmission();
	nursery.syncAwaitAllSettlements(
		sscl::ComponentThread::getSelf()->getIoContext());

	if (slotException) {
		std::rethrow_exception(slotException);
	}
}

} // namespace sscl::co

#endif // SYNC_AWAIT_NON_VIRAL_CORO_H
