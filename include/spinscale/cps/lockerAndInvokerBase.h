#ifndef LOCKER_AND_INVOKER_BASE_H
#define LOCKER_AND_INVOKER_BASE_H

#include <list>
#include <memory>

namespace sscl {

// Forward declaration
class Qutex;

/**
 * @brief LockerAndInvokerBase - Base class for lockvoking mechanism
 *
 * This base class contains the common functionality needed by Qutex,
 * including the serialized continuation reference and comparison operators.
 */
class LockerAndInvokerBase
{
public:
	/**
	 * @brief Constructor
	 * @param serializedContinuationVaddr Raw pointer to the serialized continuation
	 */
	explicit LockerAndInvokerBase(const void* serializedContinuationVaddr)
	: serializedContinuationVaddr(serializedContinuationVaddr)
	{}

	/**
	 * @brief Typedef for list of LockerAndInvokerBase shared pointers
	 */
	typedef std::list<std::shared_ptr<LockerAndInvokerBase>> List;

	/**
	 * @brief Get the iterator for this lockvoker in the specified Qutex's queue
	 * @param qutex The Qutex to get the iterator for
	 * @return Iterator pointing to this lockvoker in the Qutex's queue
	 */
	virtual List::iterator getLockvokerIteratorForQutex(Qutex& qutex) const = 0;

	/**
	 * @brief Awaken this lockvoker by posting it to its io_service
	 * @param forceAwaken If true, post even if already awake
	 */
	virtual void awaken(bool forceAwaken = false) = 0;

	/* These two are ued to iterate through the lockset of a Lockvoker in a
	 * template-erased manner. We use them in the gridlock detection algorithm.
	 */
	virtual size_t getLockSetSize() const = 0;
	virtual Qutex& getLockAt(size_t index) const = 0;

	/**
	 * @brief Equality operator
	 * 
	 * Compare by the address of the continuation objects. Why?
	 * Because there's no guarantee that the lockvoker object that was
	 * passed in by the io_service invocation is the same object as that
	 * which is in the qutexQs. Especially because we make_shared() a
	 * copy when registerInQutexQueues()ing.
	 *
	 * Generally when we "wake" a lockvoker by enqueuing it, boost's
	 * io_service::post will copy the lockvoker object.
	 */
	bool operator==(const LockerAndInvokerBase &other) const
	{
		return serializedContinuationVaddr == other.serializedContinuationVaddr;
	}

	/**
	 * @brief Inequality operator
	 */
	bool operator!=(const LockerAndInvokerBase &other) const
	{
		return serializedContinuationVaddr != other.serializedContinuationVaddr;
	}

protected:
	/* Never let this monstrosity be seen beyond this class's scope.
	 * Remember what I've taught you, quasi-modo?
	 */
	const void* serializedContinuationVaddr;
};

} // namespace sscl

#endif // LOCKER_AND_INVOKER_BASE_H
