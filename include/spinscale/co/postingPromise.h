#ifndef POSTING_PROMISE_H
#define POSTING_PROMISE_H

#include <config.h>
#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <typeinfo>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <spinscale/componentThread.h>
#include <spinscale/co/coQutex.h>
#include <spinscale/co/promiseChainLink.h>
#include <spinscale/co/promiseReturnOps.h>
#include <spinscale/co/returnValues.h>
#include <spinscale/spinLock.h>

namespace sscl::co {

template <typename PromiseType, typename T>
class Invoker;

template <typename T>
struct PostingPromise
: public PromiseChainLink
{
	struct PostBackStatus
	{
		struct CalleeFlowExecutor;
		struct CallerFlowExecutor;
		friend struct CalleeFlowExecutor;
		friend struct CallerFlowExecutor;

		explicit PostBackStatus(PostingPromise &calleePromiseIn) noexcept
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

			void operator()() noexcept
			{
				sscl::SpinLock::Guard guard(this->parent.lock);
				this->parent.calleeIsReadyToPostBack = true;
				if (this->parent.callerHasSetCallerSchedHandle)
				{
					boost::asio::post(
						this->parent.calleePromise.callerIoContext,
						this->parent.calleePromise.callerSchedHandle);
				}
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

		PostingPromise &calleePromise;

	private:
		sscl::SpinLock lock;
		bool callerHasSetCallerSchedHandle = false;
		bool calleeIsReadyToPostBack = false;
	};

	/**	Post-to must run from this awaiter's await_suspend, not synchronously inside
	 *	promise.initial_suspend() before it returns: the implementation's hidden coroutine
	 *	state (async segment / suspend index used on the next resume()) is only updated
	 *	after initial_suspend has finished returning its awaiter. Posting the handle too
	 *	early lets the callee resume before that update and re-enter initial_suspend from
	 *	the start, duplicating the post. See docs/prompts/post-to-and-back-in-invokables.md.
	 */
	struct InitialSuspendPostingInvoker
	:	public std::suspend_always
	{
		InitialSuspendPostingInvoker(
			boost::asio::io_context &targetIoContextIn,
			std::coroutine_handle<> targetSchedHandleIn) noexcept
		:	targetIoContext(targetIoContextIn),
		targetSchedHandle(targetSchedHandleIn)
		{}

		bool await_suspend(std::coroutine_handle<> const) noexcept
		{
			boost::asio::post(targetIoContext, targetSchedHandle);
			return true;
		}

		boost::asio::io_context &targetIoContext;
		std::coroutine_handle<> targetSchedHandle;
	};

	/**	Post-back (non-viral completion post; viral CalleeFlowExecutor) must run from this
	 *	awaiter's await_suspend, not synchronously inside promise.final_suspend() before it
	 *	returns: the hidden coroutine segment index in the coroutine state is only advanced
	 *	after final_suspend exits. Doing that work inside final_suspend's body risks the same
	 *	kind of ordering bug as initial_suspend—resume observing the wrong segment. See
	 *	docs/prompts/post-to-and-back-in-invokables.md.
	 */
	struct FinalSuspendPostingInvoker
	:	public std::suspend_always
	{
		explicit FinalSuspendPostingInvoker(PostingPromise &calleePromiseIn) noexcept
		:	calleePromise(calleePromiseIn)
		{}

		bool await_suspend(std::coroutine_handle<> const) noexcept
		{
			if (calleePromise.callerLambda)
			{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
				std::cout << "final_suspend" << ": " << std::this_thread::get_id()
					<< " Non-viral: posting callerLambda completion to callerIoContext.\n";
#endif
				boost::asio::post(
					calleePromise.callerIoContext,
					[&calleeRef = calleePromise]()
					{
						if (calleeRef.returnValues.myExceptionPtr) {
							std::rethrow_exception(calleeRef.returnValues.myExceptionPtr);
						}

						calleeRef.callerLambda();
					});
			}
			else
			{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
				std::cout << "final_suspend" << ": " << std::this_thread::get_id()
					<< " Viral: running CalleeFlowExecutor.\n";
#endif
				calleePromise.postBackStatus.getCalleeFlowExecutor()();
			}
			return true;
		}

		PostingPromise &calleePromise;
	};

	PostingPromise() noexcept
	: returnValues(), postBackStatus(*this)
	{}

	template <typename... TailArgs>
	PostingPromise(
		std::exception_ptr &_callerExceptionPtr,
		std::function<void()> _callerLambda,
		TailArgs &&...) noexcept
	: returnValues(_callerExceptionPtr),
	callerLambda(std::move(_callerLambda)),
	postBackStatus(*this)
	{}

	/**	Member coroutines pass the implicit object parameter before explicit
	 *	(exceptionPtr, callback, ...) args. Discard the object and delegate to
	 *	the free-function constructor shape.
	 */
	template <typename ObjectArg, typename... TailArgs>
	requires (!std::same_as<std::remove_cvref_t<ObjectArg>, std::exception_ptr>)
	PostingPromise(
		ObjectArg &&,
		std::exception_ptr &_callerExceptionPtr,
		std::function<void()> _callerLambda,
		TailArgs &&...) noexcept
	: PostingPromise(
		_callerExceptionPtr,
		std::move(_callerLambda))
	{}

	~PostingPromise() noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Destructing.\n";
#endif
	}

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

	/**	Non-viral: post completion lambda to callerIoContext from this thread.
	 *	Viral: run CalleeFlowExecutor (handshake flags); caller may post caller resume
	 *	later via CallerFlowExecutor. See docs/caller-posts-to-own-io-context.md.
	 *	Work runs in FinalSuspendPostingInvoker::await_suspend after the suspend point
	 *	advances (see docs/prompts/post-to-and-back-in-invokables.md).
	 */
	auto final_suspend() noexcept
	{
		return FinalSuspendPostingInvoker(*this);
	}

	ReturnValues<T> returnValues;
	std::function<void()> callerLambda;
	boost::asio::io_context &callerIoContext =
		sscl::ComponentThread::getSelf()->getIoContext();
	std::coroutine_handle<> selfSchedHandle;
	std::coroutine_handle<void> callerSchedHandle;
	PromiseChainLink *callerChainLink = nullptr;
	PostBackStatus postBackStatus;

protected:
	void setSelfSchedHandle(std::coroutine_handle<> schedHandle) noexcept
	{
		selfSchedHandle = schedHandle;
	}

	void setCallerPromiseChainLink(PromiseChainLink *chainLink) noexcept
	{
		callerChainLink = chainLink;
	}

	template <typename, typename>
	friend class Invoker;
};

template <typename T, typename ThreadTag>
struct TaggedPostingPromise
:	public PostingPromise<T>,
	public PromiseReturnOps<TaggedPostingPromise<T, ThreadTag>, T>
{
	TaggedPostingPromise() noexcept
	:	PostingPromise<T>()
	{}

	template <typename... TailArgs>
	TaggedPostingPromise(
		std::exception_ptr &_exceptionPtr,
		std::function<void()> _callerLambda,
		TailArgs &&... tailArgs) noexcept
	:	PostingPromise<T>(
			_exceptionPtr,
			std::move(_callerLambda),
			std::forward<TailArgs>(tailArgs)...)
	{}

	template <typename ObjectArg, typename... TailArgs>
	requires (!std::same_as<std::remove_cvref_t<ObjectArg>, std::exception_ptr>)
	TaggedPostingPromise(
		ObjectArg &&,
		std::exception_ptr &_exceptionPtr,
		std::function<void()> _callerLambda,
		TailArgs &&... tailArgs) noexcept
	:	PostingPromise<T>(
			_exceptionPtr,
			std::move(_callerLambda),
			std::forward<TailArgs>(tailArgs)...)
	{}

	auto initial_suspend() noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " About to post selfSchedHandle to " << typeid(ThreadTag).name() << ".\n";
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning InitialSuspendPostingInvoker.\n";
#endif
		return typename PostingPromise<T>::InitialSuspendPostingInvoker(
			ThreadTag::io_context(),
			this->selfSchedHandle);
	}
};

} // namespace sscl::co

#endif // POSTING_PROMISE_H
