#ifndef PROMISE_RETURN_OPS_H
#define PROMISE_RETURN_OPS_H

#include <type_traits>
#include <utility>

#include <spinscale/co/returnValues.h>

namespace sscl::co {

/**	`return_value` / `return_void` only. ThreadTag is not a template parameter here:
 *	for tagged promises, PromiseType is `TaggedPostingPromise<T, ThreadTag>`.
 */
template <typename PromiseType, typename T, bool IsVoid = std::is_void_v<T>>
struct PromiseReturnOps;

template <typename PromiseType, typename T>
struct PromiseReturnOps<PromiseType, T, false>
{
	void return_value(T returnValue) noexcept
	{
		static_cast<PromiseType *>(this)->returnValues.myReturnValue =
			std::move(returnValue);
	}
};

template <typename PromiseType, typename T>
struct PromiseReturnOps<PromiseType, T, true>
{
	void return_void() noexcept
	{
		return;
	}
};

} // namespace sscl::co

#endif // PROMISE_RETURN_OPS_H
