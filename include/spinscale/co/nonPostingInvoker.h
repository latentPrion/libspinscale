#ifndef NON_POSTING_INVOKER_H
#define NON_POSTING_INVOKER_H

#include <config.h>
#include <coroutine>
#include <iostream>
#include <thread>
#include <utility>

#include <spinscale/co/nonPostingPromise.h>

namespace sscl::co {

template <typename PromiseType>
class NonPostingInvoker
{
public:
	explicit NonPostingInvoker(PromiseType &_calleePromise) noexcept
	: calleePromise(_calleePromise)
	{}

	NonPostingInvoker(const NonPostingInvoker &) = delete;
	NonPostingInvoker &operator=(const NonPostingInvoker &) = delete;

	NonPostingInvoker(NonPostingInvoker &&other) noexcept
	: calleePromise(other.calleePromise),
	ownsFrameDestroy_(std::exchange(other.ownsFrameDestroy_, false))
	{}

	NonPostingInvoker &operator=(NonPostingInvoker &&other) = delete;

	~NonPostingInvoker() noexcept
	{
		if (!ownsFrameDestroy_) { return; }

		std::coroutine_handle<> handle = calleePromise.selfSchedHandle;
		if (handle) {
			handle.destroy();
		}
	}

	ReturnValues<void> &completedReturnValues() noexcept
		{ return calleePromise.returnValues; }

	const ReturnValues<void> &completedReturnValues() const noexcept
		{ return calleePromise.returnValues; }

private:
	PromiseType &calleePromise;

	/**	Every live invoker owns destruction of its callee coroutine frame in
	 *	~NonPostingInvoker (via calleePromise.selfSchedHandle).
	 *
	 *	The only time frame destruction is skipped is for a moved-from invoker
	 *	after move construction, so we do not double-destroy the same handle
	 *	when get_return_object() returns the invoker by value.
	 */
	bool ownsFrameDestroy_ = true;
};

} // namespace sscl::co

#endif // NON_POSTING_INVOKER_H
