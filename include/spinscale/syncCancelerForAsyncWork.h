#ifndef SYNC_CANCELER_FOR_ASYNC_WORK_H
#define SYNC_CANCELER_FOR_ASYNC_WORK_H

#include <concepts>
#include <utility>

#include <spinscale/sharedResourceGroup.h>
#include <spinscale/spinLock.h>

namespace sscl {

/**
 * SyncCancelerForAsyncWork
 *
 * A small helper to coordinate synchronous cancellation requests with
 * asynchronous work that must only observe cancellation at explicit
 * uncancelable segment boundaries.
 *
 * The async callee should structure its logic as:
 * - enter an uncancelable segment (execUncancelableSegmentOrAbort)
 * - perform synchronous work that must not be interrupted
 * - exit the segment
 * - perform cancelable async work (outside the lock)
 * - repeat
 *
 * requestStop() blocks until any currently-executing segment releases s.lock,
 * then flips shouldContinue to false. This guarantees shouldContinue is stable
 * throughout each uncancelable segment.
 *
 * Shutdown call sites that also cancel internal async operations (timers, I/O,
 * hardware capture, etc.) must call requestStop() on slot cancelers before
 * cancelling those internal operations, so callees observe stop intent when
 * the internal op unblocks their co_await.
 *
 * startAcceptingWork() is intentionally unlocked. Precondition: callers must
 * only call startAcceptingWork() when no async callee is running yet (e.g. at
 * the end of setup(), before posting/arming the first async work). If this
 * method races a running callee, that is a caller bug.
 */
class SyncCancelerForAsyncWork
{
public:
	struct Resources
	{
		bool shouldContinue = false;
	};

	SyncCancelerForAsyncWork() = default;

	void startAcceptingWork()
	{
		// Intentionally unlocked — see class-level EXPLANATION above.
		s.rsrc.shouldContinue = true;
	}

	/** @return shouldContinue before this call (was work being accepted?) */
	bool requestStop()
	{
		sscl::SpinLock::Guard guard(s.lock);
		const bool wasContinuing = s.rsrc.shouldContinue;
		s.rsrc.shouldContinue = false;
		return wasContinuing;
	}

	/** @return true if requestStop() has set shouldContinue to false. */
	bool isCancellationRequested()
	{
		sscl::SpinLock::Guard guard(s.lock);
		return isCancellationRequestedUnlocked();
	}

	bool isCancellationRequestedUnlocked() const
		{ return !s.rsrc.shouldContinue; }

	template<typename Body>
	requires std::invocable<Body>
	bool execUncancelableSegmentOrAbort(Body&& body)
	{
		sscl::SpinLock::Guard guard(s.lock);
		if (!s.rsrc.shouldContinue) {
			return false;
		}
		std::forward<Body>(body)();
		return true;
	}

public:
	sscl::SharedResourceGroup<sscl::SpinLock, Resources> s;
};

} // namespace sscl

#endif // SYNC_CANCELER_FOR_ASYNC_WORK_H
