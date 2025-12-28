#ifndef SPINSCALE_CALLBACK_H
#define SPINSCALE_CALLBACK_H

#include <memory>

namespace sscl {

// Forward declaration
class AsynchronousContinuationChainLink;

/**
 * @brief Callback class that wraps a function and its caller continuation
 * 
 * This class provides a way to pass both a callback function and the
 * caller's continuation in a single object, enabling deadlock detection
 * by walking the chain of continuations.
 *
 * Usage: Callback<CbFnT>{context, std::bind(...)}
 */
template<typename CbFnT>
class Callback
{
public:
	// Aggregate initialization allows: Callback<CbFnT>{context, std::bind(...)}
	std::shared_ptr<AsynchronousContinuationChainLink> callerContinuation;
	CbFnT callbackFn;
};

} // namespace sscl

#endif // SPINSCALE_CALLBACK_H
