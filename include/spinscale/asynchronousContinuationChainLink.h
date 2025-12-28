#ifndef ASYNCHRONOUS_CONTINUATION_CHAIN_LINK_H
#define ASYNCHRONOUS_CONTINUATION_CHAIN_LINK_H

#include <memory>

namespace sscl {

/**
 * @brief Base class for all asynchronous continuation chain links
 *
 * This non-template base class provides type erasure for the continuation
 * chain, allowing RTTI and dynamic casting when walking the chain.
 *
 * The chain walking logic can use dynamic_cast to determine the most
 * derived type and perform appropriate operations.
 *
 * Inherits from enable_shared_from_this to allow objects to obtain a
 * shared_ptr to themselves, which is useful for gridlock detection tracking.
 */
class AsynchronousContinuationChainLink
:	public std::enable_shared_from_this<AsynchronousContinuationChainLink>
{
public:
    virtual ~AsynchronousContinuationChainLink() = default;

	virtual std::shared_ptr<AsynchronousContinuationChainLink>
	getCallersContinuationShPtr() const = 0;
};

} // namespace sscl

#endif // ASYNCHRONOUS_CONTINUATION_CHAIN_LINK_H
