#ifndef INVOKERS_H
#define INVOKERS_H

#include <config.h>
#include <coroutine>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <spinscale/co/postingInvoker.h>

namespace sscl::co {

/**	Non-viral coroutine entry that must not be co_awaited: promise is always
 *	PostingPromiseTemplate<void> (no return-value path to a caller).
 *
 *	The invoker must outlive the callee frame: do not discard the return object
 *	from get_return_object(). ~PostingInvoker destroys the callee frame.
 */
template <template <typename> class PostingPromiseTemplate>
struct NonViralNonSuspendingInvoker
:	public PostingInvoker<PostingPromiseTemplate<void>, void>
{
	struct promise_type
	:	public PostingPromiseTemplate<void>
	{
		using PostingPromiseTemplate<void>::PostingPromiseTemplate;

		NonViralNonSuspendingInvoker<PostingPromiseTemplate> get_return_object()
		{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning NonViralNonSuspendingInvoker.\n";
#endif
			if (!this->callerLambda)
			{
				/**	EXPLANATION:
				 * We require a completion lambda to be provided to the
				 * non-viral coroutines, because that's how we internally
				 * distinguish between non-viral and viral coroutines.
				 *
				 * Additionally, non-viral coroutines almost never have a
				 * good reason to not have a completion lambda.
				 */
				std::ostringstream oss;
				oss << std::this_thread::get_id()
					<< ": Missing completion lambda: non-viral coroutines require a completion lambda.";
				throw std::runtime_error(oss.str());
			}

			this->setSelfSchedHandle(
				std::coroutine_handle<promise_type>::from_promise(*this));

			return NonViralNonSuspendingInvoker<PostingPromiseTemplate>(*this);
		}
	};

	using PostingInvoker<PostingPromiseTemplate<void>, void>::PostingInvoker;

	bool await_ready() const noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
#endif
		return true;
	}

	void await_suspend(std::coroutine_handle<NonViralNonSuspendingInvoker<PostingPromiseTemplate>>) noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
#endif
	}

	void await_resume() noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " This shouldn't be called.\n";
#endif
	}
};

/**	Viral awaitable: promise_type inherits PostingPromiseTemplate<T> (posting
 *	target chosen by the posting-promise alias, e.g. BodyPostingPromise<int>).
 *
 *	The invoker must outlive the callee frame until results are read.
 *	~PostingInvoker destroys the callee frame (not await_resume).
 */
template <template <typename> class PostingPromiseTemplate, typename T>
struct ViralSuspendingInvoker
:	public PostingInvoker<PostingPromiseTemplate<T>, T>
{
	struct promise_type
	:	public PostingPromiseTemplate<T>
	{
		using PostingPromiseTemplate<T>::PostingPromiseTemplate;

		ViralSuspendingInvoker<PostingPromiseTemplate, T> get_return_object() noexcept
		{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
			std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning ViralSuspendingInvoker.\n";
#endif
			this->setSelfSchedHandle(
				std::coroutine_handle<promise_type>::from_promise(*this));

			return ViralSuspendingInvoker<PostingPromiseTemplate, T>(*this);
		}
	};

	using PostingInvoker<PostingPromiseTemplate<T>, T>::PostingInvoker;

	bool await_ready() const noexcept
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Returning false.\n";
#endif
		return false;
	}

	template <typename CallerPromise>
	bool await_suspend(std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"ViralSuspendingInvoker caller promise must derive from PromiseChainLink");
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Setting callerSchedHandle.\n";
#endif
		const bool suspendCaller = this->setCallerSchedHandle(callerSchedHandle);
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " CallerFlowExecutor returned suspend=" << suspendCaller << ".\n";
#endif

		/**	EXPLANATION:
		 * If the callee was ready to post-back, then we don't need to
		 * suspend the caller -- so return either false or
		 * a symmetric transfer handle to the `callerSchedHandle` we were
		 * passed as an argument.
		 *
		 * If the callee is not ready to post-back, then we need to suspend
		 * the caller so that the caller can suspend until the callee posts
		 * the callerSchedHandle to the callerIoContext -- so return true
		 * or std::noop_coroutine().
		 */
		return suspendCaller;
	}

	T await_resume()
	{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id() << " Resumed on caller thread, hopefully.\n";
#endif
		return PostingInvoker<PostingPromiseTemplate<T>, T>::await_resume();
	}
};

} // namespace sscl::co

#endif // INVOKERS_H
