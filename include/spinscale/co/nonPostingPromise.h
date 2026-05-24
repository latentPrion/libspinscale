#ifndef NON_POSTING_PROMISE_H
#define NON_POSTING_PROMISE_H

#include <config.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <thread>
#include <utility>

#include <spinscale/spinLock.h>
#include <spinscale/co/coQutex.h>
#include <spinscale/co/promiseChainLink.h>
#include <spinscale/co/promiseReturnOps.h>
#include <spinscale/co/returnValues.h>

namespace sscl::co {

template <typename T>
struct NonPostingPromise
:	public PromiseChainLink,
	public PromiseReturnOps<NonPostingPromise<T>, T>
{
	struct PostBackStatus
	{
		struct CalleeFlowExecutor;
		struct CallerFlowExecutor;
		friend struct CalleeFlowExecutor;
		friend struct CallerFlowExecutor;

		explicit PostBackStatus(NonPostingPromise &calleePromiseIn) noexcept
		: calleePromise(calleePromiseIn)
		{}

		void reset() noexcept
		{
			sscl::SpinLock::Guard guard(lock);
			callerHasSetCallerSchedHandle = false;
			calleeIsReadyToPostBack = false;
		}

		struct FlowExecutor
		{
			explicit FlowExecutor(PostBackStatus &parentIn) noexcept
			: parent(parentIn)
			{}

			PostBackStatus &parent;
		};

		struct CalleeFlowExecutor
		:	public FlowExecutor
		{
			explicit CalleeFlowExecutor(PostBackStatus &parentIn) noexcept
			: FlowExecutor(parentIn)
			{}

			bool operator()() noexcept
			{
				sscl::SpinLock::Guard guard(this->parent.lock);
				this->parent.calleeIsReadyToPostBack = true;
				if (this->parent.callerHasSetCallerSchedHandle) {
					return true;
				}
				return false;
			}
		};

		struct CallerFlowExecutor
		:	public FlowExecutor
		{
			explicit CallerFlowExecutor(PostBackStatus &parentIn) noexcept
			: FlowExecutor(parentIn)
			{}

			bool operator()() noexcept
			{
				sscl::SpinLock::Guard guard(this->parent.lock);
				this->parent.callerHasSetCallerSchedHandle = true;
				if (this->parent.calleeIsReadyToPostBack) {
					return false;
				}
				return true;
			}
		};

		CalleeFlowExecutor getCalleeFlowExecutor() noexcept
		{
			return CalleeFlowExecutor(*this);
		}

		CallerFlowExecutor getCallerFlowExecutor() noexcept
		{
			return CallerFlowExecutor(*this);
		}

		NonPostingPromise &calleePromise;

	private:
		sscl::SpinLock lock;
		bool callerHasSetCallerSchedHandle = false;
		bool calleeIsReadyToPostBack = false;
	};

	/**	Completion work must run from this awaiter's await_suspend, not
	 *	synchronously inside promise.final_suspend() before it returns: the
	 *	hidden coroutine segment index in the coroutine state is only advanced
	 *	after final_suspend exits. See docs/prompts/post-to-and-back-in-invokables.md.
	 */
	struct FinalSuspendNonPostingInvoker
	:	public std::suspend_always
	{
		explicit FinalSuspendNonPostingInvoker(
			NonPostingPromise &calleePromiseIn) noexcept
		: calleePromise(calleePromiseIn)
		{}

		std::coroutine_handle<> await_suspend(
			std::coroutine_handle<> const) noexcept
		{
			if (calleePromise.callerLambda)
			{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
				std::cout << "final_suspend" << ": "
					<< std::this_thread::get_id()
					<< " Non-viral non-posting: invoking callerLambda directly.\n";
#endif
				if (calleePromise.returnValues.myExceptionPtr) {
					std::rethrow_exception(
						calleePromise.returnValues.myExceptionPtr);
				}

				calleePromise.callerLambda();
				return std::noop_coroutine();
			}

#ifdef CONFIG_LIBSSCL_DEBUG_CO
			std::cout << "final_suspend" << ": " << std::this_thread::get_id()
				<< " Viral non-posting: running CalleeFlowExecutor.\n";
#endif
			const bool symmetricTransferToCaller =
				calleePromise.postBackStatus.getCalleeFlowExecutor()();

			if (symmetricTransferToCaller && calleePromise.callerSchedHandle) {
				return calleePromise.callerSchedHandle;
			}

			return std::noop_coroutine();
		}

		NonPostingPromise &calleePromise;
	};

	NonPostingPromise() noexcept
	: returnValues(),
	postBackStatus(*this)
	{}

	template <typename... TailArgs>
	NonPostingPromise(
		std::exception_ptr &callerExceptionPtr,
		std::function<void()> callerLambdaIn,
		TailArgs &&...) noexcept
	: returnValues(callerExceptionPtr),
	callerLambda(std::move(callerLambdaIn)),
	postBackStatus(*this)
	{}

	template <typename ObjectArg, typename... TailArgs>
	requires (!std::same_as<std::remove_cvref_t<ObjectArg>, std::exception_ptr>)
	NonPostingPromise(
		ObjectArg &&,
		std::exception_ptr &callerExceptionPtr,
		std::function<void()> callerLambdaIn,
		TailArgs &&...) noexcept
	: NonPostingPromise(
		callerExceptionPtr,
		std::move(callerLambdaIn))
	{}

	~NonPostingPromise() noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " Destructing.\n";
#endif
	}

	std::suspend_never initial_suspend() noexcept
		{ return {}; }

	auto final_suspend() noexcept
		{ return FinalSuspendNonPostingInvoker(*this); }

	void unhandled_exception() noexcept
	{
		returnValues.myExceptionPtr = std::current_exception();
	}

	void removeAcquiredLock(CoQutex &coQutex) noexcept override
	{
		eraseFirstMatchingAcquiredLock(coQutex);
	}

	const PromiseChainLink *callerPromiseChainLink() const noexcept override
		{ return callerChainLink; }

	PromiseChainLink *callerPromiseChainLink() noexcept override
		{ return callerChainLink; }

	void setSelfSchedHandle(std::coroutine_handle<> schedHandle) noexcept
		{ selfSchedHandle = schedHandle; }

	void setCallerPromiseChainLink(PromiseChainLink *chainLink) noexcept
		{ callerChainLink = chainLink; }

	ReturnValues<T> returnValues;
	std::function<void()> callerLambda;
	PostBackStatus postBackStatus;
	std::coroutine_handle<> selfSchedHandle;
	std::coroutine_handle<> callerSchedHandle;
	PromiseChainLink *callerChainLink = nullptr;

	template <typename, typename>
	friend class NonPostingInvoker;
};

} // namespace sscl::co

#endif // NON_POSTING_PROMISE_H
