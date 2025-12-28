#ifndef SERIALIZED_ASYNCHRONOUS_CONTINUATION_H
#define SERIALIZED_ASYNCHRONOUS_CONTINUATION_H

#include <config.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <spinscale/componentThread.h>
#include <spinscale/lockSet.h>
#include <spinscale/asynchronousContinuation.h>
#include <spinscale/lockerAndInvokerBase.h>
#include <spinscale/callback.h>
#include <spinscale/qutexAcquisitionHistoryTracker.h>

namespace sscl {

template <class OriginalCbFnT>
class SerializedAsynchronousContinuation
:	public PostedAsynchronousContinuation<OriginalCbFnT>
{
public:
	SerializedAsynchronousContinuation(
		const std::shared_ptr<ComponentThread> &caller,
		Callback<OriginalCbFnT> originalCbFn,
		std::vector<std::reference_wrapper<Qutex>> requiredLocks)
	:	PostedAsynchronousContinuation<OriginalCbFnT>(caller, originalCbFn),
		requiredLocks(*this, std::move(requiredLocks))
	{}

	template<typename... Args>
	void callOriginalCb(Args&&... args)
	{
		requiredLocks.release();
		PostedAsynchronousContinuation<OriginalCbFnT>::callOriginalCb(
			std::forward<Args>(args)...);
	}

	// Return list of all qutexes in predecessors' LockSets; excludes self.
	[[nodiscard]]
	std::unique_ptr<std::forward_list<std::reference_wrapper<Qutex>>>
	getAcquiredQutexHistory() const;

	/**
	 * @brief Release a specific qutex early
	 * @param qutex The qutex to release early
	 */
	void releaseQutexEarly(Qutex &qutex)
		{ requiredLocks.releaseQutexEarly(qutex); }

public:
	LockSet<OriginalCbFnT> requiredLocks;
	std::atomic<bool> isAwakeOrBeingAwakened{false};

	/**
	 * @brief LockerAndInvoker - Template class for lockvoking mechanism
	 *
	 * This class wraps a std::bind result and provides locking functionality.
	 * When locks cannot be acquired, the object re-posts itself to the io_service
	 * queue, implementing the "spinqueueing" pattern.
	 */
	template <class InvocationTargetT>
	class LockerAndInvoker
	:	public LockerAndInvokerBase
	{
	public:
		/**
		 * @brief Constructor that immediately posts to io_service
		 * @param serializedContinuation Reference to the serialized continuation
		 *	containing LockSet and target io_service
		 * @param target The ComponentThread whose io_service to post to
		 * @param invocationTarget The std::bind result to invoke when locks are acquired
		 */
		LockerAndInvoker(
			SerializedAsynchronousContinuation<OriginalCbFnT>
				&serializedContinuation,
			const std::shared_ptr<ComponentThread>& target,
			InvocationTargetT invocationTarget)
		:	LockerAndInvokerBase(&serializedContinuation),
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
		creationTimestamp(std::chrono::steady_clock::now()),
#endif
		serializedContinuation(serializedContinuation),
		target(target),
		invocationTarget(std::move(invocationTarget))
		{
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
			std::optional<std::reference_wrapper<Qutex>> firstDuplicatedQutex =
				traceContinuationHistoryForDeadlock();

			if (firstDuplicatedQutex.has_value())
			{
				handleDeadlock(firstDuplicatedQutex.value().get());
				throw std::runtime_error(
					"LockerAndInvoker::LockerAndInvoker(): Deadlock detected");
			}
#endif // CONFIG_ENABLE_DEBUG_LOCKS

			firstWake();
		}

		/**
		 * @brief Function call operator - tries to acquire locks and either
		 * 	invokes the target or returns (already registered in qutex queues)
		 */
		void operator()();

		/**
		 * @brief Get the iterator for this lockvoker in the specified Qutex's queue
		 * @param qutex The Qutex to get the iterator for
		 * @return Iterator pointing to this lockvoker in the Qutex's queue
		 */
		LockerAndInvokerBase::List::iterator
		getLockvokerIteratorForQutex(Qutex& qutex) const override
		{
			return serializedContinuation.requiredLocks.getLockUsageDesc(
				qutex).iterator;
		}

		/**
		 * @brief Awaken this lockvoker by posting it to its io_service
		 * @param forceAwaken If true, post even if already awake
		 */
		void awaken(bool forceAwaken = false) override
		{
			bool prevVal = serializedContinuation.isAwakeOrBeingAwakened
				.exchange(true);

			if (prevVal == true && !forceAwaken)
				{ return; }

			target->getIoService().post(*this);
		}

		size_t getLockSetSize() const override
			{ return serializedContinuation.requiredLocks.locks.size(); }

		Qutex& getLockAt(size_t index) const override
		{
			return serializedContinuation.requiredLocks.locks[index]
				.qutex.get();
		}

	private:
		// Allow awakening by resetting the awake flag
		void allowAwakening()
			{ serializedContinuation.isAwakeOrBeingAwakened.store(false); }

		/**	EXPLANATION:
		 * We create a copy of the Lockvoker and then give sh_ptrs to that
		 * *COPY*, to each Qutex's internal queue. This enables us to keep
		 * the AsyncContinuation sh_ptr (which the Lockvoker contains within
		 * itself) alive without wasting too much memory.
		 *
		 * This way the io_service objects can remove the lockvoker from
		 * their queues and there'll be a copy of the lockvoker in each
		 * Qutex's queue.
		 *
		 * For non-serialized, posted continuations, they won't be removed
		 * from the io_service queue until they're executed, so there's no
		 * need to create copies of them. Lockvokers are removed from their
		 * io_service, potentially without being executed if they fail to
		 * acquire all locks.
		 */
		void registerInLockSet()
		{
			auto sharedLockvoker = std::make_shared<
				LockerAndInvoker<InvocationTargetT>>(*this);

			serializedContinuation.requiredLocks.registerInQutexQueues(
				sharedLockvoker);
		}

		/**
		 * @brief First wake - register in queues and awaken
		 * 
		 * Sets isAwake=true before calling awaken with forceAwaken to ensure
		 * that none of the locks we just registered with awaken()s a duplicate
		 * copy of this lockvoker on the io_service.
		 */
		void firstWake()
		{
			serializedContinuation.isAwakeOrBeingAwakened.store(true);
			registerInLockSet();
			// Force awaken since we just set the flag above
			awaken(true);
		}

		// Has CONFIG_DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS elapsed since creation?
		bool isDeadlockLikely() const
		{
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
			auto now = std::chrono::steady_clock::now();
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - creationTimestamp);
			return elapsed.count() >= CONFIG_DEBUG_QUTEX_DEADLOCK_TIMEOUT_MS;
#else
			return false;
#endif
		}

		// Wrapper around isDeadlockLikely for gridlock detection
		bool isGridlockLikely() const
			{ return isDeadlockLikely(); }

#ifdef CONFIG_ENABLE_DEBUG_LOCKS
		struct obsolete {
			bool traceContinuationHistoryForGridlockOn(Qutex &firstFailedQutex);
		};

		bool traceContinuationHistoryForDeadlockOn(Qutex &firstFailedQutex);
		std::optional<std::reference_wrapper<Qutex>>
		traceContinuationHistoryForDeadlock(void)
		{
			for (auto& lockUsageDesc
				: serializedContinuation.requiredLocks.locks)
			{
				if (traceContinuationHistoryForDeadlockOn(
					lockUsageDesc.qutex.get()))
				{
					return std::ref(lockUsageDesc.qutex.get());
				}
			}
			return std::nullopt;
		}

		/**
		 * @brief Handle a likely deadlock situation by logging debug information
		 * @param firstFailedQutex The first qutex that failed acquisition
		 */
		void handleDeadlock(const Qutex &firstFailedQutex)
		{
			std::cerr << __func__ << ": Deadlock: "
				<< "Lockvoker has been waiting for "
				<< std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - this->creationTimestamp)
					.count()
				<< "ms, failed on qutex @" << &firstFailedQutex
				<< " (" << firstFailedQutex.name << ")" << std::endl;
		}

		void handleGridlock(const Qutex &firstFailedQutex)
		{
			std::cerr << __func__ << ": Gridlock: "
				<< "Lockvoker has been waiting for "
				<< std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - this->creationTimestamp)
					.count()
				<< "ms, failed on qutex @" << &firstFailedQutex
				<< " (" << firstFailedQutex.name << ")" << std::endl;
		}
#endif

	private:
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
		std::chrono::steady_clock::time_point creationTimestamp;
#endif
		SerializedAsynchronousContinuation<OriginalCbFnT>
			&serializedContinuation;
		std::shared_ptr<ComponentThread> target;
		InvocationTargetT invocationTarget;
	};
};

/******************************************************************************/

#ifdef CONFIG_ENABLE_DEBUG_LOCKS

template <class OriginalCbFnT>
std::unique_ptr<std::forward_list<std::reference_wrapper<Qutex>>>
SerializedAsynchronousContinuation<OriginalCbFnT>::getAcquiredQutexHistory()
const
{
	auto heldLocks = std::make_unique<
		std::forward_list<std::reference_wrapper<Qutex>>>();

	/**	EXPLANATION:
	 * Walk through the continuation chain to collect all acquired locks
	 *
	 * We don't add the current continuation's locks because it's the one
	 * failing to acquire locks and backing off. So we start from the previous
	 * continuation.
	 */
	for (std::shared_ptr<AsynchronousContinuationChainLink> currContin =
			this->getCallersContinuationShPtr();
		 currContin != nullptr;
		 currContin = currContin->getCallersContinuationShPtr())
	{
		auto serializedCont = std::dynamic_pointer_cast<
			SerializedAsynchronousContinuation<OriginalCbFnT>>(currContin);

		if (serializedCont == nullptr) { continue; }

		// Add this continuation's locks to the held locks list
		for (size_t i = 0; i < serializedCont->requiredLocks.locks.size(); ++i)
		{
			heldLocks->push_front(serializedCont->requiredLocks.locks[i].qutex);
		}
	}

	return heldLocks;
}

template <class OriginalCbFnT>
template <class InvocationTargetT>
bool
SerializedAsynchronousContinuation<OriginalCbFnT>
::LockerAndInvoker<InvocationTargetT>
::traceContinuationHistoryForDeadlockOn(Qutex& firstFailedQutex)
{
	/**	EXPLANATION:
	 * In this function we will trace through the chain of continuations that
	 * led up to this Lockvoker's continuation. For each continuation which is
	 * a SerializedAsynchronousContinuation, we check through its LockSet to see
	 * if it contains the lock that failed acquisition. If it does, we have a
	 * deadlock.
	 */

	/* We can't start with the continuation directly referenced by this starting
	* Lockvoker as it would contain the all locks we're currently trying to
	* acquire...and rightly so because it's the continuation for this current
	* lockvoker.
	*/
	for (std::shared_ptr<AsynchronousContinuationChainLink> currContin =
			this->serializedContinuation.getCallersContinuationShPtr();
		currContin != nullptr;
		currContin = currContin->getCallersContinuationShPtr())
	{
		auto serializedCont = std::dynamic_pointer_cast<
			SerializedAsynchronousContinuation<OriginalCbFnT>>(currContin);

		if (serializedCont == nullptr) { continue; }

		// Check if the firstFailedQutex is in this continuation's LockSet
		try {
			serializedCont->requiredLocks.getLockUsageDesc(firstFailedQutex);
		} catch (const std::runtime_error& e) {
			std::cerr << __func__ << ": " << e.what() << std::endl;
			continue;
		}

		std::cout << __func__ << ":Deadlock detected: Found "
			<< "firstFailedQutex @" << &firstFailedQutex
			<< " (" << firstFailedQutex.name << ") in LockSet of "
			<< "SerializedAsynchronousContinuation @"
			<< serializedCont.get() << std::endl;

		return true;
	}

	return false;
}

template <class OriginalCbFnT>
template <class InvocationTargetT>
bool
SerializedAsynchronousContinuation<OriginalCbFnT>
::LockerAndInvoker<InvocationTargetT>
::obsolete::traceContinuationHistoryForGridlockOn(Qutex &firstFailedQutex)
{
	/**	EXPLANATION:
	 * In this function we check for gridlocks which are slightly different
	 * from deadlocks. In a gridlock, two requests are waiting for locks that
	 * are held by the other. I.e:
	 *
	 * R1 holds LockA and is waiting for LockB.
	 * R2 holds LockB and is waiting for LockA.
	 *
	 * This differs from deadlocks because it's not a single request which is
	 * attempting to re-acquire a lock that it already holds.
	 *
	 * To detect this condition, we wait until the acquisition timeout has
	 * expired. Then: we extract the current owner of the first lock we're
	 * failing to acquire.
	 *
	 * From there, we go through each of the locks in the foreign owner's
	 * current (i.e: immediate, most recent continuation's) required LockSet.
	 * For each of the locks in the foreign owner's most immediate required
	 * LockSet, we trace backward in our *OWN* history to see if any of *OUR*
	 * continuations (excluding our most immediate continuation) contains that
	 * lock.
	 *
	 * If we find a match, that means that we're holding a lock that the foreign
	 * owner is waiting for. And we already know that the foreign owner is
	 * holding a lock that we're waiting for (when we extracted the current
	 * owner of the first failed lock in our most immediate Lockset).
	 *
	 * Hence, we have a gridlock.
	 */

	std::shared_ptr<LockerAndInvokerBase> foreignOwnerShPtr =
		firstFailedQutex.getCurrOwner();
	// If no current owner, can't be a gridlock
	if (foreignOwnerShPtr == nullptr)
		{ return false; }

	// Use reference for the rest of the function for safety.
	LockerAndInvokerBase &foreignOwner = *foreignOwnerShPtr;

	/* For each lock in the foreign owner's LockSet, check if we hold it
	 * in any of our previous continuations (excluding our most immediate one)
	 */
	for (size_t i = 0; i < foreignOwner.getLockSetSize(); ++i)
	{
		Qutex& foreignLock = foreignOwner.getLockAt(i);

		/* Skip the firstFailedQutex since we already know the foreign owner
		 * holds it -- hence it's impossible for any of our previous
		 * continuations to hold it.
		 */
		if (&foreignLock == &firstFailedQutex)
			{ continue; }

		/**	EXPLANATION:
		 * Trace backward through our continuation history (excluding our most
		 * immediate continuation).
		 *
		 * The reason we exclude our most immediate continuation is because the
		 * LockSet acquisition algorithm backs off if it fails to acquire ALL
		 * locks in the set. So if the lock that the foreign owner is waiting
		 * for is in our most immediate continuation, and NOT in one of our
		 * previous continuations, then we will back off and the foreign owner
		 * should eventually be able to acquire that lock.
		 */
		for (std::shared_ptr<AsynchronousContinuationChainLink> currContin =
				this->serializedContinuation.getCallersContinuationShPtr();
			 currContin != nullptr;
			 currContin = currContin->getCallersContinuationShPtr())
		{
			auto serializedCont = std::dynamic_pointer_cast<
				SerializedAsynchronousContinuation<OriginalCbFnT>>(currContin);

			if (serializedCont == nullptr) { continue; }

			// Check if this continuation holds the foreign lock
			try {
				const auto& lockUsageDesc = serializedCont->requiredLocks
					.getLockUsageDesc(foreignLock);

				// Matched! We hold a lock that the foreign owner is waiting for
				std::cout << __func__ << ": Gridlock detected: We hold lock @"
					<< &foreignLock << " (" << foreignLock.name << ") in "
					"continuation @" << serializedCont.get()
					<< ", while foreign owner @" << &foreignOwner
					<< " holds lock @" << &firstFailedQutex << " ("
					<< firstFailedQutex.name << ") that we're waiting for"
					<< std::endl;

				return true;
			} catch (const std::runtime_error& e) {
				// This continuation doesn't hold the foreign lock. Continue.
				continue;
			}
		}
	}

	return false;
}

#endif // CONFIG_ENABLE_DEBUG_LOCKS

template <class OriginalCbFnT>
template <class InvocationTargetT>
void SerializedAsynchronousContinuation<OriginalCbFnT>
::LockerAndInvoker<InvocationTargetT>::operator()()
{
	if (ComponentThread::getSelf() != target)
	{
		throw std::runtime_error(
			"LockerAndInvoker::operator(): Thread safety violation - "
			"executing on wrong ComponentThread");
	}

	std::optional<std::reference_wrapper<Qutex>> firstFailedQutexRet;
	bool deadlockLikely = isDeadlockLikely();
	bool gridlockLikely = isGridlockLikely();

	if (!serializedContinuation.requiredLocks.tryAcquireOrBackOff(
		*this, firstFailedQutexRet))
	{
		// Just allow this lockvoker to be dropped from its io_service.
		allowAwakening();
		if (!deadlockLikely && !gridlockLikely)
			{ return; }

#ifdef CONFIG_ENABLE_DEBUG_LOCKS
		Qutex	&firstFailedQutex = firstFailedQutexRet.value().get();
		bool isDeadlock = traceContinuationHistoryForDeadlockOn(
			firstFailedQutex);

		bool gridlockIsHeuristicallyLikely = false;
		bool gridlockIsAlgorithmicallyLikely = false;

		if (gridlockLikely)
		{
			auto& tracker = QutexAcquisitionHistoryTracker
				::getInstance();

			auto heldLocks = serializedContinuation
				.getAcquiredQutexHistory();

			// Add this continuation to the tracker
			auto currentContinuationShPtr = serializedContinuation
				.shared_from_this();

			tracker.addIfNotExists(
				currentContinuationShPtr,
				firstFailedQutex, std::move(heldLocks));

			gridlockIsHeuristicallyLikely = tracker
				.heuristicallyTraceContinuationHistoryForGridlockOn(
					firstFailedQutex, currentContinuationShPtr);

			if (gridlockIsHeuristicallyLikely)
			{
				gridlockIsAlgorithmicallyLikely = tracker
					.completelyTraceContinuationHistoryForGridlockOn(
						firstFailedQutex);
			}
		}

		bool isGridlock = (gridlockIsHeuristicallyLikely
			|| gridlockIsAlgorithmicallyLikely);

		if (!isDeadlock && !isGridlock)
			{ return; }

		if (isDeadlock) { handleDeadlock(firstFailedQutex); }
		if (isGridlock) { handleGridlock(firstFailedQutex); }
#endif
		return;
	}

	/**	EXPLANATION:
	 * Successfully acquired all locks, so unregister from qutex queues.
	 * We do this here so that we can free up queue slots in the qutex
	 * queues for other lockvokers that may be waiting to acquire the
	 * locks. The size of the qutex queues does matter for other
	 * contending lockvokers; and so also does their position in the
	 * queues.
	 *
	 * The alternative is to leave ourself in the queues until we
	 * eventually release all locks; and given that we may hold locks
	 * even across true async hardware bottlenecks, this could take a
	 * long time.
	 *
	 * Granted, the fact that we own the locks means that even though
	 * we've removed ourselves from the queues, other lockvokers still
	 * can't acquire the locks anyway.
	 */
	serializedContinuation.requiredLocks.unregisterFromQutexQueues();

#ifdef CONFIG_ENABLE_DEBUG_LOCKS
	/**	EXPLANATION:
	 * If we were being tracked for gridlock detection but successfully
	 * acquired all locks, it was a false positive due to timed delay,
	 * long-running operation, or I/O delay
	 */
	if (gridlockLikely)
	{
		std::shared_ptr<AsynchronousContinuationChainLink>
			currentContinuationShPtr =
				serializedContinuation.shared_from_this();

		bool removed = QutexAcquisitionHistoryTracker::getInstance()
			.remove(currentContinuationShPtr);

		if (removed)
		{
			std::cerr
				<< "LockerAndInvoker::operator(): False positive "
				"gridlock detection - continuation @"
				<< &serializedContinuation
				<< " was being tracked but successfully acquired all "
				"locks. This was likely due to timed delay, "
				"long-running operation, or I/O delay."
				<< std::endl;
		}
	}
#endif

	invocationTarget();
}

} // namespace sscl

#endif // SERIALIZED_ASYNCHRONOUS_CONTINUATION_H
