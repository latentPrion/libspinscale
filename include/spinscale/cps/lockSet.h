#ifndef LOCK_SET_H
#define LOCK_SET_H

#include <vector>
#include <stdexcept>
#include <utility>
#include <memory>
#include <optional>
#include <spinscale/cps/qutex.h>
#include <spinscale/cps/lockerAndInvokerBase.h>

namespace sscl::cps {

class Qutex;

/**
 * @brief LockSet - Manages a collection of locks for acquisition/release
 *
 * LockSet exists only because the CPS re-enqueuing model had no way to acquire
 * locks in a fine-grained way. A LockerAndInvoker could re-post only the entire
 * continuation, and only before that continuation began executing; there was no
 * mechanism to re-enqueue individual segments within a continuation. The
 * practical consequence was that all required Qutexes had to be acquired at
 * once up front, before the continuation body could run at all.
 *
 * releaseQutexEarly() was a partial workaround for finer-grained control, but
 * it only helped on the release side and did not solve the fundamental problem
 * of acquiring locks one-at-a-time mid-sequence.
 *
 * co::CoQutex supersedes this abstraction: coroutines can co_await individual
 * locks at the points where they are actually needed, which is the finer control
 * LockSet and releaseQutexEarly() were aiming for with limited success.
 */
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
	 * @param qutexes Vector of Qutex references that must be acquired
	 */
	explicit LockSet(std::vector<std::reference_wrapper<Qutex>> qutexes = {})
	: allLocksAcquired(false), registeredInQutexQueues(false)
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
	 * and especially not all those that aren't even in any io_context queues.
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

	std::optional<std::reference_wrapper<LockUsageDesc>>
	findLockUsageDesc(const Qutex &criterionLock)
	{
		for (auto& lockUsageDesc : locks)
		{
			if (&lockUsageDesc.qutex.get() == &criterionLock) {
				return std::ref(lockUsageDesc);
			}
		}

		return std::nullopt;
	}

	std::optional<std::reference_wrapper<const LockUsageDesc>>
	findLockUsageDesc(const Qutex &criterionLock) const
	{
		for (const auto& lockUsageDesc : locks)
		{
			if (&lockUsageDesc.qutex.get() == &criterionLock) {
				return std::cref(lockUsageDesc);
			}
		}

		return std::nullopt;
	}

	LockUsageDesc &getLockUsageDesc(const Qutex &criterionLock)
	{
		auto lockUsageDesc = findLockUsageDesc(criterionLock);
		if (lockUsageDesc.has_value()) {
			return lockUsageDesc->get();
		}

		// Should never happen if the LockSet is properly constructed
		throw std::runtime_error(
			std::string(__func__) +
			": Qutex not found in this LockSet");
	}

	const LockUsageDesc &getLockUsageDesc(const Qutex &criterionLock) const
	{
		auto lockUsageDesc = findLockUsageDesc(criterionLock);
		if (lockUsageDesc.has_value()) {
			return lockUsageDesc->get();
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

		auto& lockUsageDesc = getLockUsageDesc(qutex);

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
	bool allLocksAcquired, registeredInQutexQueues;
};

} // namespace sscl::cps

#endif // LOCK_SET_H
