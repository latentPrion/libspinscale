#ifndef LOCK_SET_H
#define LOCK_SET_H

#include <vector>
#include <stdexcept>
#include <utility>
#include <memory>
#include <optional>
#include <spinscale/qutex.h>
#include <spinscale/lockerAndInvokerBase.h>

namespace sscl {

// Forward declarations
template <class OriginalCbFnT>
class SerializedAsynchronousContinuation;
class Qutex;

/**
 * @brief LockSet - Manages a collection of locks for acquisition/release
 */
template <class OriginalCbFnT>
class LockSet
{
public:
	/**	EXPLANATION:
	 * Tracks both the Qutex that must be acquired, as well as the parent
	 * LockerAndInvoker that this LockSet has registered into that Qutex's
	 * queue.
	 */
	struct LockUsageDesc
	{
		std::reference_wrapper<Qutex> qutex;
		typename LockerAndInvokerBase::List::iterator iterator;
		bool hasBeenReleased = false;

		LockUsageDesc(std::reference_wrapper<Qutex> qutexRef,
			typename LockerAndInvokerBase::List::iterator iter)
			: qutex(qutexRef), iterator(iter), hasBeenReleased(false) {}
	};

	typedef std::vector<std::reference_wrapper<Qutex>> Set;

public:
	/**
	 * @brief Constructor
	 * @param parentContinuation Reference to the parent
	 * 	SerializedAsynchronousContinuation
	 * @param qutexes Vector of Qutex references that must be acquired
	 */
	LockSet(
		SerializedAsynchronousContinuation<OriginalCbFnT> &parentContinuation,
		std::vector<std::reference_wrapper<Qutex>> qutexes = {})
	: parentContinuation(parentContinuation), allLocksAcquired(false),
	registeredInQutexQueues(false)
	{
		/* Convert Qutex references to LockUsageDesc (iterators will be filled
		 * in during registration)
		 */
		locks.reserve(qutexes.size());
		for (auto& qutexRef : qutexes)
		{
			locks.emplace_back(
				qutexRef,
				typename LockerAndInvokerBase::List::iterator{});
		}
	}

	/**
	 * @brief Register the LockSet with all its Qutex locks
	 * @param lockvoker The LockerAndInvoker to register with each Qutex
	 *
	 *	EXPLANATION:
	 * I'm not sure an unregisterFromQutexQueues() method is needed.
	 * Why? Because if an async sequence can't acquire all locks, it will
	 * simply never leave the qutexQ until it eventually does. The only other
	 * time it will leave the qutexQ is when the program terminates.
	 *
	 * I'm not sure we'll actually cancal all in-flight async sequences --
	 * and especially not all those that aren't even in any io_service queues.
	 * To whatever extent these objects get cleaned up, they'll probably be
	 * cleaned up in the qutexQ's std::list destructor -- and that won't
	 * execute any fancy cleanup logic. It'll just clear() out the list.
	 */
	void registerInQutexQueues(
		const std::shared_ptr<LockerAndInvokerBase> &lockvoker
		)
	{
		/**	EXPLANATION:
		 * Register the lockvoker with each Qutex and store the returned
		 * iterator to its place within each Qutex's queue. We store the
		 * iterator so that we can quickly move the lockvoker around within
		 * the queue, and eventually, erase() it when we acquire all the
		 * locks.
		 */
		for (auto& lockUsageDesc : locks)
		{
			lockUsageDesc.iterator = lockUsageDesc.qutex.get().registerInQueue(
				lockvoker);
		}

		registeredInQutexQueues = true;
	}

	void unregisterFromQutexQueues()
	{
		if (!registeredInQutexQueues)
		{
			throw std::runtime_error(
				std::string(__func__) +
				": LockSet::unregisterFromQutexQueues() called but not "
				"registered in Qutex queues");
		}

		// Unregister from all qutex queues
		for (auto& lockUsageDesc : locks)
		{
			auto it = lockUsageDesc.iterator;
			lockUsageDesc.qutex.get().unregisterFromQueue(it);
		}
	}


	/**
	 * @brief Try to acquire all locks in order; back off if acquisition fails
	 * @param lockvoker The LockerAndInvoker attempting to acquire the locks
	 * @param firstFailedQutex Output parameter to receive the first Qutex that
	 * 	failed acquisition (can be nullptr)
	 * @return true if all locks were acquired, false otherwise
	 */
	bool tryAcquireOrBackOff(
		LockerAndInvokerBase &lockvoker,
		std::optional<std::reference_wrapper<Qutex>> &firstFailedQutex
			= std::nullopt
		)
	{
		if (!registeredInQutexQueues)
		{
			throw std::runtime_error(
				std::string(__func__) +
				": LockSet::tryAcquireOrBackOff() called but not registered in "
				"Qutex queues");
		}
		if (allLocksAcquired)
		{
			throw std::runtime_error(
				std::string(__func__) +
				": LockSet::tryAcquireOrBackOff() called but allLocksAcquired "
				"is already true");
		}

		// Try to acquire all required locks
		int nAcquired = 0;
		const int nRequiredLocks = static_cast<int>(locks.size());
		for (auto& lockUsageDesc : locks)
		{
			if (!lockUsageDesc.qutex.get().tryAcquire(
				lockvoker, nRequiredLocks))
			{
				// Set the first failed qutex for debugging
				firstFailedQutex = std::ref(lockUsageDesc.qutex.get());
				break;
			}

			nAcquired++;
		}

		if (nAcquired < nRequiredLocks)
		{
			// Release any locks we managed to acquire
			for (int i = 0; i < nAcquired; i++) {
				locks[i].qutex.get().backoff(lockvoker, nRequiredLocks);
			}

			return false;
		}

		allLocksAcquired = true;
		return true;
	}

	// @brief Release all locks
	void release()
	{
		if (!registeredInQutexQueues)
		{
			throw std::runtime_error(
				std::string(__func__) +
				": LockSet::release() called but not registered in Qutex "
				"queues");
		}

		if (!allLocksAcquired)
		{
			throw std::runtime_error(
				std::string(__func__) +
				": LockSet::release() called but allLocksAcquired is false");
		}

		for (auto& lockUsageDesc : locks)
		{
			if (lockUsageDesc.hasBeenReleased) { continue; }

			lockUsageDesc.qutex.get().release();
		}

		allLocksAcquired = false;
	}

	const LockUsageDesc &getLockUsageDesc(const Qutex &criterionLock) const
	{
		for (auto& lockUsageDesc : locks)
		{
			if (&lockUsageDesc.qutex.get() == &criterionLock) {
				return lockUsageDesc;
			}
		}

		// Should never happen if the LockSet is properly constructed
		throw std::runtime_error(
			std::string(__func__) +
			": Qutex not found in this LockSet");
	}

	/**
	 * @brief Release a specific qutex early and mark it as released
	 * @param qutex The qutex to release early
	 */
	void releaseQutexEarly(Qutex &qutex)
	{
		if (!allLocksAcquired)
		{
			throw std::runtime_error(
				std::string(__func__) +
				": LockSet::releaseQutexEarly() called but allLocksAcquired is false");
		}

		auto& lockUsageDesc = const_cast<LockUsageDesc&>(
			getLockUsageDesc(qutex));

		if (!lockUsageDesc.hasBeenReleased)
		{
			lockUsageDesc.qutex.get().release();
			lockUsageDesc.hasBeenReleased = true;
		}

		return;
	}

public:
	std::vector<LockUsageDesc> locks;

private:
	SerializedAsynchronousContinuation<OriginalCbFnT> &parentContinuation;
	bool allLocksAcquired, registeredInQutexQueues;
};

} // namespace sscl

#endif // LOCK_SET_H
