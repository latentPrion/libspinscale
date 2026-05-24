#ifndef INVOKER_BASE_H
#define INVOKER_BASE_H

#include <config.h>
#include <coroutine>
#include <iostream>
#include <thread>
#include <type_traits>
#include <utility>

#include <spinscale/co/promiseChainLink.h>
#include <spinscale/co/returnValues.h>

namespace sscl::co {

template <typename PromiseType, typename T>
class PostingInvoker
{
public:
	explicit PostingInvoker(PromiseType &_calleePromise) noexcept
	: calleePromise(_calleePromise)
	{}

	PostingInvoker(const PostingInvoker &) = delete;
	PostingInvoker &operator=(const PostingInvoker &) = delete;

	PostingInvoker(PostingInvoker &&other) noexcept
	: calleePromise(other.calleePromise),
	ownsFrameDestroy_(std::exchange(other.ownsFrameDestroy_, false))
	{}

	PostingInvoker &operator=(PostingInvoker &&other) = delete;

	~PostingInvoker() noexcept
	{
		if (!ownsFrameDestroy_) { return; }

		std::coroutine_handle<> handle = calleePromise.selfSchedHandle;
		if (handle) {
			handle.destroy();
		}
	}

	template <typename CallerPromise>
	bool setCallerSchedHandle(
		std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"PostingInvoker caller promise must derive from PromiseChainLink");

		calleePromise.callerSchedHandle = callerSchedHandle;
		calleePromise.setCallerPromiseChainLink(&callerSchedHandle.promise());
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " Done setting callerSchedHandle; running CallerFlowExecutor.\n";
#endif
		return calleePromise.postBackStatus.getCallerFlowExecutor()();
	}

	ReturnValues<T> &completedReturnValues() noexcept
		{ return calleePromise.returnValues; }

	const ReturnValues<T> &completedReturnValues() const noexcept
		{ return calleePromise.returnValues; }

	auto await_resume()
	{
		calleePromise.postBackStatus.reset();

		ReturnValues<T> &returnValues = calleePromise.returnValues;
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " About to check for and rethrow any exception.\n";
#endif

		if (returnValues.myExceptionPtr)
		{
			std::exception_ptr const captured = returnValues.myExceptionPtr;
			std::rethrow_exception(captured);
		}
		if constexpr (!std::is_void_v<T>)
		{
			T result = std::move(returnValues.myReturnValue);
			return result;
		}
	}

private:
	PromiseType &calleePromise;

	/**	EXPLANATION:
	 * Every live invoker owns destruction of its callee coroutine frame in
	 * ~PostingInvoker (via calleePromise.selfSchedHandle).
	 *
	 * The only time frame destruction is skipped is for a moved-from invoker
	 * after move construction or move assignment, so we do not double-destroy
	 * the same handle when get_return_object() returns the invoker by value.
	 *
	 * This is not an opt-out for viral vs non-viral callers or for "callee
	 * still running"; callers must keep the invoker alive until the callee
	 * frame is no longer needed.
	 */
	bool ownsFrameDestroy_ = true;
};

template <typename PromiseType, typename T>
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

	template <typename CallerPromise>
	bool setCallerSchedHandle(
		std::coroutine_handle<CallerPromise> callerSchedHandle) noexcept
	{
		static_assert(
			std::is_base_of_v<PromiseChainLink, CallerPromise>,
			"NonPostingInvoker caller promise must derive from PromiseChainLink");

		calleePromise.callerSchedHandle = callerSchedHandle;
		calleePromise.setCallerPromiseChainLink(
			&callerSchedHandle.promise());
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " Done setting callerSchedHandle; running CallerFlowExecutor.\n";
#endif
		return calleePromise.postBackStatus.getCallerFlowExecutor()();
	}

	ReturnValues<T> &completedReturnValues() noexcept
		{ return calleePromise.returnValues; }

	const ReturnValues<T> &completedReturnValues() const noexcept
		{ return calleePromise.returnValues; }

	auto await_resume()
	{
		calleePromise.postBackStatus.reset();

		ReturnValues<T> &returnValues = calleePromise.returnValues;
#ifdef CONFIG_LIBSSCL_DEBUG_CO
		std::cout << __func__ << ": " << std::this_thread::get_id()
			<< " About to check for and rethrow any exception.\n";
#endif

		if (returnValues.myExceptionPtr) {
			std::exception_ptr const captured = returnValues.myExceptionPtr;
			std::rethrow_exception(captured);
		}
		if constexpr (!std::is_void_v<T>)
		{
			T result = std::move(returnValues.myReturnValue);
			return result;
		}
	}

protected:
	PromiseType &calleePromise;

private:
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

#endif // INVOKER_BASE_H
