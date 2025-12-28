#ifndef QUTEX_ACQUISITION_HISTORY_TRACKER_H
#define QUTEX_ACQUISITION_HISTORY_TRACKER_H

#include <unordered_map>
#include <memory>
#include <forward_list>
#include <functional>
#include "spinLock.h"


namespace sscl {

// Forward declarations
class Qutex;
class AsynchronousContinuationChainLink;
class DependencyGraph;

/**
 * @brief QutexAcquisitionHistoryTracker - Tracks acquisition history for
 *        gridlock detection
 *
 * This class maintains a central acquisition history to track all lockvokers
 * suspected of being gridlocked. It stores information about what locks each
 * timed-out lockvoker wants and what locks they hold in their continuation
 * history.
 */
class QutexAcquisitionHistoryTracker
{
public:
	/**
	 * @brief Type definition for the acquisition history entry
	 *
	 * pair.first: The firstFailedQutex that this lockvoker WANTS but can't
	 * acquire
	 * pair.second: A unique_ptr to a list of all acquired Qutexes in this
	 * lockvoker's continuation history
	 */
	typedef std::pair<
		std::reference_wrapper<Qutex>,
		std::unique_ptr<std::forward_list<std::reference_wrapper<Qutex>>>
	> AcquisitionHistoryEntry;

	/**
	 * @brief Type definition for the acquisition history map
	 *
	 * Key: std::shared_ptr<AsynchronousContinuationChainLink>
	 *		(the continuation that contains the timed-out lockvoker)
	 * Value: AcquisitionHistoryEntry
	 *		(its wanted lock (aka: firstFailedQutex/pair.first) + held locks)
	 */
	typedef std::unordered_map<
		std::shared_ptr<AsynchronousContinuationChainLink>,
		AcquisitionHistoryEntry
	> AcquisitionHistoryMap;

public:
	static QutexAcquisitionHistoryTracker& getInstance()
	{
		static QutexAcquisitionHistoryTracker instance;
		return instance;
	}

	/**
	 * @brief Add a continuation to the acquisition history if it doesn't
	 *	already exist
	 * @param continuation Shared pointer to the
	 *	AsynchronousContinuationChainLink
	 * @param wantedLock The lock that this continuation wants but can't
	 *	acquire
	 * @param heldLocks Unique pointer to list of locks held in this
	 *	continuation's history (will be moved)
	 */
	void addIfNotExists(
		std::shared_ptr<AsynchronousContinuationChainLink> &continuation,
		Qutex& wantedLock,
		std::unique_ptr<std::forward_list<std::reference_wrapper<Qutex>>>
			heldLocks
		)
	{
		acquisitionHistoryLock.acquire();

		auto it = acquisitionHistory.find(continuation);
		// If a continuation already exists, don't add it again
		if (it != acquisitionHistory.end())
		{
			acquisitionHistoryLock.release();
			return;
		}

		acquisitionHistory.emplace(continuation, std::make_pair(
			std::ref(wantedLock), std::move(heldLocks)));

		acquisitionHistoryLock.release();
	}

	/**
	 * @brief Remove a continuation from the acquisition history
	 *
	 * @param continuation Shared pointer to the
	 *        AsynchronousContinuationChainLink to remove
	 * @return true if the continuation was found and removed, false if not found
	 */
	bool remove(
		std::shared_ptr<AsynchronousContinuationChainLink> &continuation
		)
	{
		acquisitionHistoryLock.acquire();

		auto it = acquisitionHistory.find(continuation);
		if (it != acquisitionHistory.end())
		{
			acquisitionHistory.erase(it);

			acquisitionHistoryLock.release();
			return true;
		}

		acquisitionHistoryLock.release();
		return false;
	}

	bool heuristicallyTraceContinuationHistoryForGridlockOn(
		Qutex &firstFailedQutex,
		std::shared_ptr<AsynchronousContinuationChainLink>&
			currentContinuation);
	bool completelyTraceContinuationHistoryForGridlockOn(
		Qutex &firstFailedQutex);

	/**
	 * @brief Generates a dependency graph among known continuations, based on
	 * the currently known acquisition history. There may well be a cyclical
	 * dependency which hasn't been reported to the history tracker yet.
	 * @param dontAcquireLock If true, skips acquiring the internal spinlock
	 * (assumes caller already holds it)
	 */
	[[nodiscard]] std::unique_ptr<DependencyGraph> generateGraph(
		bool dontAcquireLock = false);

	// Disable copy constructor and assignment operator
	QutexAcquisitionHistoryTracker(
		const QutexAcquisitionHistoryTracker&) = delete;
	QutexAcquisitionHistoryTracker& operator=(
		const QutexAcquisitionHistoryTracker&) = delete;

private:
	QutexAcquisitionHistoryTracker() = default;
	~QutexAcquisitionHistoryTracker() = default;

private:
	/**	EXPLANATION:
	 * We use a SpinLock here instead of a Qutex because this acquisition
	 * history tracker is invoked within the LockerAndInvoker.
	 * Since LockerAndInvoker is too tightly coupled with Qutex workings, using
	 * a Qutex here would create a circular dependency or deadlock situation.
	 * Therefore, it's best to use a SpinLock on the history class to avoid
	 * these coupling issues.
	 */
	SpinLock acquisitionHistoryLock;
	AcquisitionHistoryMap acquisitionHistory;
};

} // namespace sscl

#endif // QUTEX_ACQUISITION_HISTORY_TRACKER_H
