#ifndef PROMISE_CHAIN_LINK_H
#define PROMISE_CHAIN_LINK_H

#include <functional>
#include <list>

namespace sscl::co {

class CoQutex;

/**
 * Non-template base for coroutine promises participating in a logical
 * promise chain (analogous to libspinscale AsynchronousContinuationChainLink).
 * A future deadlock detector can walk callerPromiseChainLink() without
 * knowing concrete promise_type.
 */
class PromiseChainLink
{
public:
	virtual ~PromiseChainLink() = default;

	/** Reserved for deadlock detection: link toward the caller / outer coroutine. */
	virtual const PromiseChainLink *callerPromiseChainLink() const noexcept
		{ return nullptr; }
	virtual PromiseChainLink *callerPromiseChainLink() noexcept
		{ return nullptr; }

	void addAcquiredLock(CoQutex &coQutex) noexcept
		{ acquiredLocks.emplace_back(std::ref(coQutex)); }

	bool holdsAcquiredLock(const CoQutex &coQutex) const noexcept
		{ return findMatchingAcquiredLock(coQutex) != acquiredLocks.end(); }

	virtual void removeAcquiredLock(CoQutex &coQutex) noexcept = 0;

protected:
	using AcquiredLockList = std::list<std::reference_wrapper<CoQutex>>;

	AcquiredLockList::iterator findMatchingAcquiredLock(CoQutex &coQutex) noexcept
	{
		for (auto it = acquiredLocks.begin(); it != acquiredLocks.end(); ++it) {
			if (&it->get() == &coQutex) {
				return it;
			}
		}
		return acquiredLocks.end();
	}

	AcquiredLockList::const_iterator findMatchingAcquiredLock(const CoQutex &coQutex) const noexcept
	{
		for (auto it = acquiredLocks.begin(); it != acquiredLocks.end(); ++it) {
			if (&it->get() == &coQutex) {
				return it;
			}
		}
		return acquiredLocks.end();
	}

	void eraseFirstMatchingAcquiredLock(CoQutex &coQutex) noexcept
	{
		auto match = findMatchingAcquiredLock(coQutex);
		if (match != acquiredLocks.end()) {
			acquiredLocks.erase(match);
		}
	}

	AcquiredLockList acquiredLocks;
};

} // namespace sscl::co

#endif // PROMISE_CHAIN_LINK_H
