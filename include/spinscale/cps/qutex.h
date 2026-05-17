#ifndef QUTEX_H
#define QUTEX_H

#include <config.h>
#include <list>
#include <memory>
#include <string>
#include <spinscale/spinLock.h>
#include <spinscale/lockerAndInvokerBase.h>

namespace sscl {

/**
 * @brief Qutex - Queue-based mutex for asynchronous lock management
 *
 * A Qutex combines a spinlock, an ownership flag, and a queue of waiting
 * lockvokers to provide efficient asynchronous lock management with
 * priority-based acquisition for LockSets.
 */
class Qutex
{
public:
	/**
	 * @brief Constructor
	 */
	Qutex([[maybe_unused]] const std::string &_name)
	:
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
	name(_name), currOwner(nullptr),
#endif
	isOwned(false)
	{}

	/**
	 * @brief Register a lockvoker in the queue
	 * @param lockvoker The lockvoker to register
	 * @return Iterator pointing to the registered lockvoker in the queue
	 */
	LockerAndInvokerBase::List::iterator registerInQueue(
		const std::shared_ptr<LockerAndInvokerBase> &lockvoker
		)
	{
		lock.acquire();
		auto it = queue.insert(queue.end(), lockvoker);
		lock.release();
		return it;
	}

	/**
	 * @brief Unregister a lockvoker from the queue
	 * @param it Iterator pointing to the lockvoker to unregister
	 * @param shouldLock Whether to acquire the spinlock before erasing (default: true)
	 */
	void unregisterFromQueue(
		LockerAndInvokerBase::List::iterator it, bool shouldLock = true
		)
	{
		if (shouldLock)
		{
			lock.acquire();
			queue.erase(it);
			lock.release();
		}
		else {
			queue.erase(it);
		}
	}

	/**
	 * @brief Try to acquire the lock for a lockvoker
	 * @param tryingLockvoker The lockvoker attempting to acquire the lock
	 * @param nRequiredLocks Number of locks required by the lockvoker's LockSet
	 * @return true if the lock was successfully acquired, false otherwise
	 */
	bool tryAcquire(
		const LockerAndInvokerBase &tryingLockvoker, int nRequiredLocks);

	/**
	 * @brief Handle backoff when a lockvoker fails to acquire all required locks
	 * @param failedAcquirer The lockvoker that failed to acquire all locks
	 * @param nRequiredLocks Number of locks required by the lockvoker's LockSet
	 */
	void backoff(const LockerAndInvokerBase &failedAcquirer, int nRequiredLocks);

	/**
	 * @brief Release the lock and wake up the next waiting lockvoker
	 */
	void release();

#ifdef CONFIG_ENABLE_DEBUG_LOCKS
	std::shared_ptr<LockerAndInvokerBase> getCurrOwner() const
		{ return currOwner; }
#endif

public:
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
	std::string name;
	std::shared_ptr<LockerAndInvokerBase> currOwner;
#endif
	SpinLock lock;
	LockerAndInvokerBase::List queue;
	bool isOwned;
};

} // namespace sscl

#endif // QUTEX_H
