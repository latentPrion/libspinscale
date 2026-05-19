#ifndef NON_POSTING_PROMISE_H
#define NON_POSTING_PROMISE_H

#include <config.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <thread>
#include <utility>

#include <spinscale/co/coQutex.h>
#include <spinscale/co/promiseChainLink.h>
#include <spinscale/co/promises.h>

namespace sscl::co {

struct NonPostingPromise
:	public PromiseChainLink
{
	/**	Completion work must run from this awaiter's await_suspend, not
	 *	synchronously inside promise.final_suspend() before it returns: the
	 *	hidden coroutine segment index in the coroutine state is only advanced
	 *	after final_suspend exits. See docs/prompts/post-to-and-back-in-invokables.md.
	 */
	struct FinalSuspendNonPostingInvoker
	:	public std::suspend_always
	{
		explicit FinalSuspendNonPostingInvoker(NonPostingPromise &calleePromiseIn) noexcept
		:	calleePromise(calleePromiseIn)
		{}

		bool await_suspend(std::coroutine_handle<> const) noexcept
		{
			if (calleePromise.callerLambda)
			{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
				std::cout << "final_suspend" << ": " << std::this_thread::get_id()
					<< " Non-viral non-posting: invoking callerLambda directly.\n";
#endif
				if (calleePromise.returnValues.myExceptionPtr) {
					std::rethrow_exception(
						calleePromise.returnValues.myExceptionPtr);
				}

				calleePromise.callerLambda();
			}
			return true;
		}

		NonPostingPromise &calleePromise;
	};

	NonPostingPromise() noexcept
	: returnValues()
	{}

	template <typename... TailArgs>
	NonPostingPromise(
		std::exception_ptr &callerExceptionPtr,
		std::function<void()> callerLambdaIn,
		TailArgs &&...) noexcept
	: returnValues(callerExceptionPtr),
	callerLambda(std::move(callerLambdaIn))
	{}

	~NonPostingPromise() noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Destructing.\n";
#endif
	}

	std::suspend_never initial_suspend() noexcept
		{ return {}; }

	auto final_suspend() noexcept
		{ return FinalSuspendNonPostingInvoker(*this); }

	void return_void() noexcept
		{ return; }

	void unhandled_exception() noexcept
	{
		returnValues.myExceptionPtr = std::current_exception();
	}

	void removeAcquiredLock(CoQutex &coQutex) noexcept override
	{
		eraseFirstMatchingAcquiredLock(coQutex);
	}

	void setSelfSchedHandle(std::coroutine_handle<> schedHandle) noexcept
	{
		selfSchedHandle = schedHandle;
	}

	ReturnValues<void> returnValues;
	std::function<void()> callerLambda;
	std::coroutine_handle<> selfSchedHandle;

	template <typename>
	friend class NonPostingInvoker;
};

} // namespace sscl::co

#endif // NON_POSTING_PROMISE_H
