#include <spinscale/qutexAcquisitionHistoryTracker.h>
#include <spinscale/serializedAsynchronousContinuation.h>
#include <spinscale/qutex.h>
#include <spinscale/dependencyGraph.h>
#include <memory>
#include <forward_list>
#include <functional>
#include <iostream>
#include <algorithm>

namespace sscl {

void DependencyGraph::addNode(const Node& node)
{
	adjacencyList[node]; // Creates empty set if node doesn't exist
}

void DependencyGraph::addEdge(const Node& source, const Node& target)
{
	addNode(source);
	addNode(target);
	adjacencyList[source].insert(target);
}

std::vector<std::vector<DependencyGraph::Node>>
DependencyGraph::findCycles() const
{
	std::unordered_set<Node> visited;
	std::unordered_set<Node> recursionStack;
	std::vector<std::vector<Node>> cycles;
	std::vector<Node> path;

	for (const auto& entry : adjacencyList)
	{
		const Node& node = entry.first;
		if (visited.find(node) == visited.end()) {
			dfsCycleDetection(node, visited, recursionStack, path, cycles);
		}
	}

	return cycles;
}

bool DependencyGraph::hasCycles() const
{
	std::unordered_set<Node> visited;
	std::unordered_set<Node> recursionStack;
	std::vector<std::vector<Node>> cycles;
	std::vector<Node> path;

	for (const auto& entry : adjacencyList)
	{
		const Node& node = entry.first;
		if (visited.find(node) == visited.end())
		{
			dfsCycleDetection(node, visited, recursionStack, path, cycles);
			if (!cycles.empty())
				{ return true; }
		}
	}

	return false;
}

size_t DependencyGraph::getNodeCount() const
{
	return adjacencyList.size();
}

void DependencyGraph::dfsCycleDetection(
	const Node& node,
	std::unordered_set<Node>& visited,
	std::unordered_set<Node>& recursionStack,
	std::vector<Node>& path,
	std::vector<std::vector<Node>>& cycles
	)
	const
{
	// Mark current node as visited and add to recursion stack
	visited.insert(node);
	recursionStack.insert(node);
	path.push_back(node);

	// Check all adjacent nodes
	auto it = adjacencyList.find(node);
	if (it != adjacencyList.end())
	{
		for (const auto& adjacent : it->second)
		{
			// If adjacent node is in recursion stack, we found a cycle
			if (recursionStack.find(adjacent) != recursionStack.end())
			{
				// Find the start of the cycle in the current path
				auto cycleStart = std::find(path.begin(), path.end(), adjacent);
				if (cycleStart != path.end())
				{
					std::vector<Node> cycle(cycleStart, path.end());
					cycle.push_back(adjacent); // Complete the cycle
					cycles.push_back(cycle);
				}
			}
			// If adjacent node hasn't been visited, recurse
			else if (visited.find(adjacent) == visited.end())
			{
				dfsCycleDetection(
					adjacent, visited, recursionStack, path, cycles);
			}
		}
	}

	// Remove from recursion stack and path when backtracking
	recursionStack.erase(node);
	path.pop_back();
}

// QutexAcquisitionHistoryTracker implementation
std::unique_ptr<DependencyGraph>
QutexAcquisitionHistoryTracker::generateGraph(bool dontAcquireLock)
{
	auto graph = std::make_unique<DependencyGraph>();

	if (!dontAcquireLock) {
		acquisitionHistoryLock.acquire();
	}

	// First pass: Add all continuations as nodes
	for (const auto& entry : acquisitionHistory)
	{
		const auto& continuation = entry.first;
		graph->addNode(continuation);
	}

	// Second pass: Add edges based on lock dependencies
	for (const auto& entry : acquisitionHistory)
	{
		const auto& continuation = entry.first;
		const auto& historyEntry = entry.second;
		const auto& wantedLock = historyEntry.first;
		const auto& heldLocks = historyEntry.second;

		if (!heldLocks) { continue; }

		// Check if any other continuation holds the lock this continuation wants
		for (const auto& otherEntry : acquisitionHistory)
		{
			const auto& otherContinuation = otherEntry.first;
			const auto& otherHistoryEntry = otherEntry.second;
			const auto& otherHeldLocks = otherHistoryEntry.second;

			// Skip self-comparison
			if (continuation == otherContinuation) { continue; }
			if (!otherHeldLocks) { continue; }

			// Check if other continuation holds the wanted lock
			for (const auto& otherHeldLock : *otherHeldLocks)
			{
				if (&otherHeldLock.get() == &wantedLock.get())
				{
					// Add edge: continuation -> otherContinuation
					// (continuation wants a lock held by otherContinuation)
					graph->addEdge(continuation, otherContinuation);
					break;
				}
			}
		}
	}

	if (!dontAcquireLock) {
		acquisitionHistoryLock.release();
	}

	return graph;
}

/**	EXPLANATION - GRIDLOCK DETECTION ALGORITHM:
 * This file implements gridlock detection algorithms that use a central
 * acquisition history to track all lockvokers suspected of being gridlocked.
 *
 *	ALGORITHM OVERVIEW:
 * 1. When a lockvoker finds that DEADLOCK_TIMEOUT_MS has elapsed and it
 *    still can't acquire a particular lock (firstFailedQutex), it creates
 *    a new entry in a global acquisition history.
 *
 * 2. The acquisition history is an unordered_map with:
 *    - Key: std::shared_ptr<AsynchronousContinuationChainLink>
 *		(the timed-out lockvoker's continuation)
 *    - Value: std::pair<
 *					std::reference_wrapper<Qutex>,
 *					std::unique_ptr<std::forward_list<std::reference_wrapper<Qutex>>>>
 *      * pair.first: The firstFailedQutex that this lockvoker WANTS but
 *			can't acquire. This metadata is essential for later-arriving
 *			entrants to analyze what their predecessor timed-out sequences
 *			want.
 *      * pair.second: A unique_ptr to a list of all acquired Qutexes in this
 *			lockvoker's continuation history.
 *
 * 3. Each timed-out lockvoker:
 *    a) Adds itself to the acquisition history map with its wanted lock and
 *		acquired locks
 *    b) Iterates through all OTHER entries in the map (excluding itself)
 *    c) For each other entry, checks if that entry's acquired locks
 *		(pair.second) contains the lock that this lockvoker wants
 *		(aka: firstFailedQutex/pair.first)
 *    d) If found, we have detected a gridlock: two sequences where at least
 *		one wants a lock held by the other, and the other wants a lock that
 *		it can't acquire.
 *
 *	GRIDLOCK CONDITION:
 * A gridlock exists when we find a circular chain of dependencies:
 * - Lockvoker A wants LockX but can't acquire it (held by Lockvoker B)
 * - Lockvoker B wants LockY but can't acquire it (held by Lockvoker C, D, etc.)
 * - The chain must be circular (eventually leading back to Lockvoker A or another
 *   lockvoker in the chain) to ensure it's a true gridlock, not just a delay
 *
 *	TIMED DELAY, I/O DELAY, or LONG-RUNNING OPERATION FALSE-POSITIVE:
 * Without circularity detection, we could incorrectly flag a simple delay, I/O
 * delay, or long-running operation as a gridlock. For example: Lockvoker A
 * wants LockX (held by Lockvoker B), and Lockvoker B is currently in a 10-second
 * sleep/delay. When B wakes up, it will release LockX, allowing A to proceed.
 * This is not a gridlock - it's just A waiting longer than DEADLOCK_TIMEOUT_MS
 * for B to finish its work. True gridlocks require circular dependencies where
 * no sequence can make progress because they're all waiting for each other in
 * a cycle.
 *
 * The central history metadata enables us to detect complex gridlocks involving
 * multiple lockvokers (2, 3, 4, 5+ sequences) by building up the acquisition
 * history over time as different lockvokers timeout and add their information.
 */

bool QutexAcquisitionHistoryTracker
::heuristicallyTraceContinuationHistoryForGridlockOn(
	Qutex &firstFailedQutex,
	std::shared_ptr<AsynchronousContinuationChainLink>& currentContinuation)
{
	/**	HEURISTIC APPROACH:
	 * Due to the computational complexity of full circularity detection,
	 * we implement a heuristically adequate check: when we find 2 sequences
	 * where one depends on the other, and the other has reached timeout,
	 * we assume this is a likely gridlock. This approach is not
	 * algorithmically complete (it may miss some complex circular
	 * dependencies or flag false positives), but it is heuristically useful
	 * for debugging and identifying potential concurrency issues in
	 * practice.
	 *
	 * See the file-local comment above for the complete algorithm
	 * explanation.
	 */

	/**	NOTICE:
	 * Generally we should have all global data structures owned by a single
	 * ComponentThread; and qutexes really should only be used to serialize
	 * async sequences being enqueued on the same ComponentThread. But this
	 * doesn't prevent multiple CPUs from trying to add/remove entries to/from
	 * the acquisition history at the same time. Why? The acquisition history
	 * isn't per-CPU, it's global.
	 *
	 * The problem with using a SpinLock here is that if the STL uses mutexes
	 * internally to lock containers, we could end up in a situation where
	 * spinning waiters will be busy-spinning while the owner is sleeping?
	 *
	 * But this should not happen since the nature of the order of operations is
	 * that the spinlock ensures that only one CPU at a time can be
	 * adding/removing entries; and thus everytime an method is called on the
	 * unordered_map, the caller will always succeed at acquiring the underlying
	 * STL mutex.
	 *
	 * So it should be safe to use a SpinLock here.
	 */
	acquisitionHistoryLock.acquire();

	// Iterate through all entries in the acquisition history
	for (const auto& entry : acquisitionHistory)
	{
		const auto& continuation = entry.first;
		const auto& historyEntry = entry.second;

		// Skip the current continuation (don't compare with itself)
		if (continuation == currentContinuation) {
			continue;
		}

		// Check if firstFailedQutex is in this continuation's held locks
		const std::unique_ptr<std::forward_list<std::reference_wrapper<Qutex>>>&
			heldLocks = historyEntry.second;

		if (!heldLocks)
			{ continue; }

		for (const auto& heldLock : *heldLocks)
		{
			/* Found firstFailedQutex in another continuation's held locks
			 * This indicates a potential gridlock
			 */
			if (&heldLock.get() != &firstFailedQutex)
				{ continue; }

			acquisitionHistoryLock.release();

			std::cerr << __func__ << ": GRIDLOCK DETECTED: Current "
				"continuation @" << currentContinuation.get()
				<< " wants lock '" << firstFailedQutex.name
				<< "' which is held by continuation @"
				<< continuation.get() << std::endl;

			return true;
		}
	}

	acquisitionHistoryLock.release();
	return false;
}

bool QutexAcquisitionHistoryTracker
::completelyTraceContinuationHistoryForGridlockOn(Qutex &firstFailedQutex)
{
	(void)firstFailedQutex;

	/** ALGORITHMICALLY COMPLETE VERSION:
	 * This function implements the algorithmically complete version of gridlock
	 * detection that performs full circularity detection. It builds a dependency
	 * graph from the acquisition history and uses DFS with cycle detection to
	 * identify true circular dependencies.
	 *
	 * See the file-local comment above for the complete algorithm explanation.
	 */

	acquisitionHistoryLock.acquire();

	// Helper function to print continuation dependency info
	auto printContinuationDependency = [&](
		const auto& fromContinuation, const auto& toContinuation
		)
	{
		auto it = acquisitionHistory.find(fromContinuation);
		if (it != acquisitionHistory.end())
		{
			const auto& wantedLock = it->second.first;
			std::cerr << "    Continuation @" << fromContinuation.get()
				<< " wants lock[\"" << wantedLock.get().name << "\"], "
				<< "held by continuation @" << toContinuation.get()
				<< std::endl;
		}
		else
		{
			std::cerr << "    Continuation @" << fromContinuation.get()
				<< " -> continuation @" << toContinuation.get()
				<< std::endl;
		}
	};

	// Pass true to dontAcquireLock since we already hold it
	auto graph = generateGraph(true);

	// Early return if no graph or no cycles
	if (!graph || !graph->hasCycles())
	{
		acquisitionHistoryLock.release();
		return false;
	}

	auto cycles = graph->findCycles();

	std::cerr << __func__ << ": CIRCULAR DEPENDENCIES DETECTED: Found "
		<< cycles.size() << " cycle(s) in lock dependency graph:" << std::endl;

	for (size_t i = 0; i < cycles.size(); ++i)
	{
		const auto& cycle = cycles[i];
		std::cerr << "  Cycle " << (i + 1) << ":\n";

		for (size_t j = 0; j < cycle.size() - 1; ++j)
		{
			const auto& currentContinuation = cycle[j];
			const auto& nextContinuation = cycle[j + 1];
			printContinuationDependency(currentContinuation, nextContinuation);
		}

		if (cycle.empty())
			{ continue; }

		/* Handle the last edge (back to start of cycle)
		 */
		const auto& lastContinuation = cycle[cycle.size() - 1];
		const auto& firstContinuation = cycle[0];
		printContinuationDependency(lastContinuation, firstContinuation);
	}

	acquisitionHistoryLock.release();

	return true;
}

} // namespace sscl
