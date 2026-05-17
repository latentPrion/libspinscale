#ifndef PROMISE_CHAIN_WALKER_H
#define PROMISE_CHAIN_WALKER_H

#include <cstddef>

#include <spinscale/co/promiseChainLink.h>

namespace sscl::co {

/**
 * Upper bound on caller-chain links visited after the root (guards cycles / bugs).
 *
 * Design posture (cf. docs/3rdParty/smo/libspinscale — continuation tracing vs
 * interpretation): this header performs trace-only walks along
 * PromiseChainLink::callerPromiseChainLink(); deadlock interpretation stays at
 * call sites / later policy.
 */
inline constexpr std::size_t kMaxCallerPromiseChainTraversalSteps = 4096;

inline const PromiseChainLink *nextOnCallerPromiseChain(
	const PromiseChainLink &link) noexcept
{
	return link.callerPromiseChainLink();
}

inline bool callerChainHopUnderStepLimit(
	std::size_t hopIndex) noexcept
{
	return hopIndex < kMaxCallerPromiseChainTraversalSteps;
}

template <typename Visitor>
void walkCallerPromiseChainFrom(
	const PromiseChainLink &root, Visitor &&visitor)
{
	visitor(root);
	const PromiseChainLink *next = nextOnCallerPromiseChain(root);
	for (std::size_t hopIndex = 0;
		next != nullptr && callerChainHopUnderStepLimit(hopIndex);
		++hopIndex)
	{
		visitor(*next);
		next = nextOnCallerPromiseChain(*next);
	}
}

} // namespace sscl::co

#endif // PROMISE_CHAIN_WALKER_H
