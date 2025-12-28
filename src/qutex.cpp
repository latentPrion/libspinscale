#include <spinscale/qutex.h>
#include <spinscale/lockerAndInvokerBase.h>

namespace sscl {

bool Qutex::tryAcquire(
	const LockerAndInvokerBase &tryingLockvoker, int nRequiredLocks
	)
{
	lock.acquire();

	const int qNItems = static_cast<int>(queue.size());

	// If queue is empty, this should never happen since we register before trying to acquire
	if (qNItems < 1)
	{
		lock.release();

		throw std::runtime_error(
			std::string(__func__) + 
			": tryAcquire called on empty queue - this should never happen");
	}

	// If lock is already owned, fail
	if (isOwned)
	{
		lock.release();
		return false;
	}

	/**	EXPLANATION:
	 * Calculate how many items from the rear we need to scan
	 *
	 * For nRequiredLocks=1: must be at front (nRearItemsToScan = qNItems, scan all)
	 * For nRequiredLocks=2: must be in top 50% (nRearItemsToScan = qNItems/2)
	 * For nRequiredLocks=3: must be in top 66% (nRearItemsToScan = qNItems/3)
	 * etc.
	 */
	const int nRearItemsToScan = qNItems / nRequiredLocks;

	// If we're the only item in queue, or if the fraction calculation
	// results in 0 rear items to scan, we automatically succeed
	if (qNItems == 1 || nRearItemsToScan < 1)
	{
		isOwned = true;
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
		// Use the stored iterator from the LockSet
		auto it = tryingLockvoker.getLockvokerIteratorForQutex(*this);
		currOwner = *it;
#endif
		lock.release();
		return true;
	}

	// For single-lock requests, they must be at the front of the queue
	if (nRequiredLocks == 1)
	{
		bool ret = false;

		if ((*queue.front()) == tryingLockvoker)
		{
			isOwned = true;
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
			currOwner = queue.front();
#endif
			ret = true;
		}
		else {
			ret = false;
		}

		lock.release();
		return ret;
	}

	// For multi-lock requests, check if the lockvoker is in the rear portion
	// If it's NOT in the rear portion, then it's in the top X% and should succeed
	auto rIt = queue.rbegin();
	auto rEndIt = queue.rend();
	bool foundInRear = false;

	for (int i = 0; i < nRearItemsToScan && rIt != rEndIt; ++rIt, ++i)
	{
		if ((**rIt) == tryingLockvoker)
		{
			foundInRear = true;
			break;
		}
	}

	if (foundInRear)
	{
		// Found in rear portion - not in top X%, so fail
		lock.release();
		return false;
	}

	// Not found in rear portion - must be in top X%, so succeed
	isOwned = true;
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
	// Use the stored iterator from the LockSet
	auto it = tryingLockvoker.getLockvokerIteratorForQutex(*this);
	currOwner = *it;
#endif
	lock.release();
	return true;
}

void Qutex::backoff(
	const LockerAndInvokerBase &failedAcquirer, int nRequiredLocks
	)
{
	lock.acquire();

	const int nQItems = static_cast<int>(queue.size());
	
	if (nQItems < 1)
	{
		lock.release();

		throw std::runtime_error(
			std::string(__func__) +
			": backoff called on empty queue - this should never happen");
	}

	/* Check if failedAcquirer is at the front of the queue with
	* nRequiredLocks == 1. This should never happen because an
	* acquirer at the front of the queue with nRequiredLocks == 1
	* should always succeed.
	*/
	const LockerAndInvokerBase& oldFront = *queue.front();
	if (oldFront == failedAcquirer && nRequiredLocks == 1)
	{
		lock.release();

		throw std::runtime_error(
			std::string(__func__) +
			": Failed acquirer is at front of queue with nRequiredLocks==1 - "
			"acquirer at front of queue with nRequiredLocks==1 should always "
			"succeed.");
	}

	// Rotate queue members if failedAcquirer is at front of queue
	if (oldFront == failedAcquirer && nQItems > 1)
	{
		/**	EXPLANATION:
		 * Rotate the top LockSet.size() items in the queue by moving
		 * the failedAcquirer to the last position in the top
		 * LockSet.size() items within the queue.
		 *
		 * I.e: if queue.size()==20, and lockSet.size()==5, then move
		 * failedAcquirer from the front to the 5th position in the queue,
		 * which should push the other 4 items forward.
		 * If queue.size()==3 and LockSet.size()==5, then just
		 * push_back(failedAcquirer).
		 *
		 * It is impossible for a Qutex queue to have only one
		 * item in it, yet for that Lockvoker item to have failed to
		 * acquire the Qutex. Being the only item in the ticketQ
		 * means that you must succeed at acquiring the Qutex.
		 */
		int indexOfItemToInsertCurrFrontBefore;
		if (nQItems > nRequiredLocks) {
			indexOfItemToInsertCurrFrontBefore = nRequiredLocks;
		} else
		{
			// -1 means insert at back -- i.e, use list::end() as insertPos.
			indexOfItemToInsertCurrFrontBefore = -1;
		}

		/*	EXPLANATION:
		 * Rotate them here.
		 *
		 * The reason why we do this rotation is to avoid a particular kind
		 * of deadlock wherein a grid of async requests is perfectly
		 * configured so as to guarantee that none of them can make any
		 * forward progress unless they get reordered.
		 *
		 * Consider 2 different locks with 2 different items in them
		 * each, both of which come from 2 particular requests:
		 *	Qutex1: Lockvoker1, Lv2
		 *	Qutex2: Lv2, Lv1
		 *
		 * Moreover, both of these lockvokers have requiredLocks.size()==2,
		 * and the particular 2 locks that each one requires are indeed
		 * Qutex1 and Qutex2.
		 *
		 * This particular setup basically means that in TL1's queue, Lv1
		 * will wakeup since it's at the front of TL1. It'll successfully
		 * acquire TL1 (since it's at the front), and then it'll try to
		 * acquire TL2. But since Lv1 isn't in the top 50% of items in TL2's
		 * queue, Lv1 will fail to acquire TL2.
		 *
		 * Then similarly, in TL2's queue, Lv2 will wakeup since it's at
		 * the front. Again, it'll successfully acquire TL2 since it's at
		 * the front of TL2's queue. But then it'll try to acquire TL1.
		 * Since it's not in the top 50% of TL1's enqueued items, it'll fail
		 * to acquire TL1.
		 *
		 * N.B: This type of perfectly ordered deadlock can occur in any
		 * kind of NxN situation where ticketQ.size()==requiredLocks.size().
		 * That could be 4x4, 5x5, 6x6, etc. It doesn't happen in 1x1
		 * because a Lockvoker that only requires one lock will always just
		 * succeed if it's at the front of its queue.
		 *
		 * This state of affairs is stable and will persist unless these
		 * queues are reordered in some way. Hence: that's why we rotate the
		 * items in a QutexQ after backing off of it. Backing off means
		 * Not necessarily that the calling LockVoker failed to acquire
		 * THIS PARTICULAR Qutex, but rather than it failed to acquire
		 * ALL of its required locks.
		 *
		 * Hence, if we are backing out, we should also rotate the items
		 * in the queue if the current front item is the failed acquirer.
		 * So that's why we do this rotation here.
		 */

		// Find the iterator for the failed acquirer (which is at the front)
		auto frontIt = queue.begin();

		// Find the position to insert before using indexOfItemToInsertCurrFrontBefore
		auto insertPos = queue.begin();
		if (indexOfItemToInsertCurrFrontBefore == -1)
		{
			// -1 means insert at the back (before end())
			insertPos = queue.end();
		}
		else
		{
			// Move to the specified position (0-based index)
			for (
				int i = 0;
				i < indexOfItemToInsertCurrFrontBefore
					&& insertPos != queue.end(); ++i)
			{
				++insertPos;
			}
		}

		/**	NOTE:
		 * According to https://en.cppreference.com/w/cpp/container/list/splice:
		 *	"No iterators or references become invalidated. If *this and other
		 *	refer to different objects, the iterators to the transferred elements
		 *	now refer into *this, not into other."
		 *
		 * So our stored iterator inside of LockSet will still be valid after
		 * the splice, and we can use it to unregister the lockvoker later on.
		 */
		queue.splice(insertPos, queue, frontIt);
	}

	isOwned = false;
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
	currOwner = nullptr;
#endif
	LockerAndInvokerBase &newFront = *queue.front();

	lock.release();

	/**	EXPLANATION:
	 * Why should this never happen? Well, if we were at the front of the queue
	 * and we failed to acquire the lock, we should have been rotated away from
	 * the front. On the other hand, if we were not at the front of the queue
	 * and we failed to acquire the lock, then we weren't at the front of the
	 * queue to begin with.
	 * The exception is if the queue has only one item in it.
	 *
	 * Hence there ought to be no way for the failedAcquirer to be at the front
	 * of the queue at this point UNLESS the queue has only one item in it.
	 */
	if (newFront == failedAcquirer && nQItems > 1)
	{
		throw std::runtime_error(
			std::string(__func__) +
			": Failed acquirer is at the front of the queue at the end of "
			"backoff, yet nQItems > 1 - this should never happen");
	}

	/**	EXPLANATION:
	 * We should always awaken whoever is at the front of the queue, even if
	 * we didn't rotate. Why? Consider this scenario:
	 *
	 *	Lv1 has LockSet.size==1. Lv2 has LockSet.size==3.
	 *	Lv1's required lock overlaps with Lv2's set of 3 required locks.
	 *	Lv1 registers itself in its 1 qutex's queue.
	 *	Lv2 registers itself in all 3 of its qutexes' queues.
	 *	Lv2 acquires the lock that it needs in common with Lv1.
	 *		(Assume that Lv2 was not at the front of the common qutex's
	 *		internal queue -- it only needed to be in the top 66%.)
	 *	Lv1 tries to acquire the common lock and fails. It gets taken off of
	 *		its io_service. It's now asleep until it gets
	 *		re-added into an io_service.
	 *	Lv2 fails to acquire the other 2 locks it needs and backoff()s from
	 *		the common lock it shares with Lv1.
	 *
	 *	If Lv2 does NOT awaken the item at the front of the common lock's
	 *	queue (aka: Lv1), then Lv1 is doomed to never wake up again.
	 *
	 * Hence: backout() callers should always wake up the lockvoker at the
	 * front of their queue before leaving.
	 *
	 * The exception is if the item at the front is the backout() caller
	 * itself. This can happen if, for example a multi-locking lockvoker
	 * is backing off of a qutex within which it's the only waiter.
	 */
	if (nQItems > 1) {
		newFront.awaken();
	}
}

void Qutex::release()
{
	lock.acquire();

	/**	EXPLAINATION:
	 * A qutex must not have its release() called when it's not owned. The
	 * plumbing required to permit that is a bit excessive, and we have
	 * instrumentation to track early qutex release()ing in
	 * SerializedAsynchronousContinuation.
	 */
	if (!isOwned
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
		|| currOwner == nullptr
#endif
	)
	{
		throw std::runtime_error(
			std::string(__func__) +
			": release() called on unowned qutex - this should never happen");
	}

	isOwned = false;
#ifdef CONFIG_ENABLE_DEBUG_LOCKS
	currOwner = nullptr;
#endif

	// It's possible for there to be 0 items left in queue after unregistering.
	if (queue.empty())
	{
		lock.release();
		return;
	}

	/** EXPLANATION:
	 * It would be nice to be able to optimize by only awakening if the
	 * release()ing lockvoker was at the front of the qutexQ, but if we
	 * don't unconditionally wakeup() the front item, we could get lost
	 * wakeups. Consider:
	 *
	 *	Lv1 only has 1 requiredLock.
	 *	Lv2 has 3 requiredLocks. One of its requiredLocks overlaps with
	 *		Lv1's single requiredLock. So they both share a common lock.
	 *	Lv3's currently owns Lv1 & Lv2's common requiredLock.
	 *	Lv3 release()s that common lock.
	 *	Lv1 happens to be next in queue after Lv3 unregisters itself.
	 *	Lv3 wakes up Lv1.
	 *	Just before Lv1 can acquire the common lock, Lv2 acquires it now,
	 *		because it only needs to be in the top 66% to succeed.
	 *	Lv1 checks the currOwner and sees that it's owned. Lv1 is now
	 *		dequeued from its io_service. It won't be awakened until someone
	 *		awakens it.
	 *	Lv2 finishes its critical section and releas()es the common lock.
	 *	Lv2 was not at the front of the qutexQ, so it does NOT awaken the
	 *		current item at the front.
	 *
	 * Thus, Lv1 never gets awakened again. The end.
	 * This also means that no LockSet.size()==1 lockvoker will ever be able
	 * to run again since they can only run if they are at the front of the
	 * qutexQ.
	 *
	 * Therefore we must always awaken the front item when releas()ing.
	 */
	LockerAndInvokerBase &front = *queue.front();

	lock.release();

	front.awaken();
}

} // namespace sscl
