#ifndef DEPENDENCY_GRAPH_H
#define DEPENDENCY_GRAPH_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

namespace sscl {

// Forward declarations
class AsynchronousContinuationChainLink;

/**
 * @brief DependencyGraph - Represents a directed graph for lock dependency analysis
 *
 * This graph represents dependencies between continuations (lockvokers) where
 * an edge from A to B means that continuation A wants a lock that is held by
 * continuation B. This is used to detect circular dependencies (gridlocks).
 */
class DependencyGraph
{
public:
	typedef std::shared_ptr<AsynchronousContinuationChainLink> Node;
	// Each node maps to a set of nodes it depends on
	typedef std::unordered_map<Node, std::unordered_set<Node>> AdjacencyList;

public:
	void addNode(const Node& node);

	/**
	 * @brief Add a directed edge from source to target
	 * @param source The continuation that wants a lock
	 * @param target The continuation that holds the wanted lock
	 */
	void addEdge(const Node& source, const Node& target);

	/**
	 * @brief Find all cycles in the graph using DFS
	 * @return Vector of cycles, where each cycle is a vector of nodes
	 */
	std::vector<std::vector<Node>> findCycles() const;

	/**
	 * @brief Check if there are any cycles in the graph
	 * @return true if cycles exist, false otherwise
	 */
	bool hasCycles() const;

	/**
	 * @brief Get the number of nodes in the graph
	 * @return Number of nodes
	 */
	size_t getNodeCount() const;

	/**
	 * @brief Get the adjacency list for debugging
	 * @return Reference to the adjacency list
	 */
	const AdjacencyList& getAdjacencyList() const { return adjacencyList; }

private:
	/**
	 * @brief DFS helper for cycle detection
	 * @param node Current node being visited
	 * @param visited Set of nodes that have been fully processed
	 * @param recursionStack Set of nodes currently in the recursion stack
	 * @param path Current path being explored
	 * @param cycles Vector to store found cycles
	 */
	void dfsCycleDetection(
		const Node& node,
		std::unordered_set<Node>& visited,
		std::unordered_set<Node>& recursionStack,
		std::vector<Node>& path,
		std::vector<std::vector<Node>>& cycles)
		const;

private:
	AdjacencyList adjacencyList;
};

} // namespace sscl

#endif // DEPENDENCY_GRAPH_H
