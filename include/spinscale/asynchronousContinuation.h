#ifndef ASYNCHRONOUS_CONTINUATION_H
#define ASYNCHRONOUS_CONTINUATION_H

#include <functional>
#include <memory>
#include <exception>
#include <spinscale/componentThread.h>
#include <spinscale/callback.h>
#include <spinscale/callableTracer.h>
#include <spinscale/asynchronousContinuationChainLink.h>


namespace sscl {

/**
 * AsynchronousContinuation - Template base class for async sequence management
 *
 * This template provides a common pattern for managing asynchronous operations
 * that need to maintain object lifetime through a sequence of callbacks.
 *
 * The template parameter OriginalCbFnT represents the signature of the original
 * callback that will be invoked when the async sequence completes.
 */
template <class OriginalCbFnT>
class AsynchronousContinuation
:	public AsynchronousContinuationChainLink
{
public:
	explicit AsynchronousContinuation(Callback<OriginalCbFnT> originalCb)
	: originalCallback(std::move(originalCb))
	{}

	/**		EXPLANATION:
	 * Each numbered segmented sequence persists the lifetime of the
	 * continuation object by taking a copy of its shared_ptr.
	 */
	typedef void (SegmentFn)(
		std::shared_ptr<AsynchronousContinuation<OriginalCbFnT>>
			lifetimePreservingConveyance);

	/**	EXPLANATION:
	 * When an exception is thrown in a an async callee, which pertains to an
	 * error in the data given by the caller, we ought not to throw the
	 * exception within the callee. Instead, we should store the exception
	 * in the continuation object and return it to the caller.
	 *
	 * The caller should then call checkException() to rethrow it on its
	 * own stack.
	 *
	 * This macro should be used by the caller to bubble the exception to the
	 * caller.
	 */
	#define CALLEE_SETEXC(continuation, type, exc_obj) \
		(continuation)->exception = std::make_exception_ptr<type>(exc_obj)

	#define CALLEE_SETEXC_CALLCB(continuation, type, exc_obj) \
		do { \
			CALLEE_SETEXC(continuation, type, exc_obj); \
			(continuation)->callOriginalCb(); \
		} while(0)

	#define CALLEE_SETEXC_CALLCB_RET(continuation, type, exc_obj) \
		do { \
			CALLEE_SETEXC_CALLCB(continuation, type, exc_obj); \
			return; \
		} while(0)

	// Call this in the caller to rethrow the exception.
	void checkException()
	{
		if (exception)
			{ std::rethrow_exception(exception); }
	}

	// Implement the virtual method from AsynchronousContinuationChainLink
	virtual std::shared_ptr<AsynchronousContinuationChainLink>
	getCallersContinuationShPtr() const override
		{ return originalCallback.callerContinuation; }

public:
	Callback<OriginalCbFnT> originalCallback;
	std::exception_ptr exception;
};

/**
 * NonPostedAsynchronousContinuation - For continuations that don't post
 * callbacks
 *
 * Note: We intentionally do not create a
 * LockedNonPostedAsynchronousContinuation because the only way to implement
 * non-posted locking would be via busy-spinning or sleeplocks. This would
 * eliminate the throughput advantage from our Qspinning mechanism, which
 * relies on re-posting to the io_service queue when locks are unavailable.
 */
template <class OriginalCbFnT>
class NonPostedAsynchronousContinuation
:	public AsynchronousContinuation<OriginalCbFnT>
{
public:
	explicit NonPostedAsynchronousContinuation(
		Callback<OriginalCbFnT> originalCb)
	:	AsynchronousContinuation<OriginalCbFnT>(originalCb)
	{}

	/**
	 * @brief Call the original callback with perfect forwarding
	 * (immediate execution)
	 *
	 * This implementation calls the original callback immediately without
	 * posting to any thread or queue. Used for non-posted continuations.
	 *
	 * @param args Arguments to forward to the original callback
	 */
	template<typename... Args>
	void callOriginalCb(Args&&... args)
	{
		if (AsynchronousContinuation<OriginalCbFnT>::originalCallback
			.callbackFn)
		{
			AsynchronousContinuation<OriginalCbFnT>::originalCallback
				.callbackFn(std::forward<Args>(args)...);
		}
	}
};

template <class OriginalCbFnT>
class PostedAsynchronousContinuation
:	public AsynchronousContinuation<OriginalCbFnT>
{
public:
	PostedAsynchronousContinuation(
		const std::shared_ptr<ComponentThread> &caller,
		Callback<OriginalCbFnT> originalCbFn)
	:	AsynchronousContinuation<OriginalCbFnT>(originalCbFn),
	caller(caller)
	{}

	template<typename... Args>
	void callOriginalCb(Args&&... args)
	{
		if (AsynchronousContinuation<OriginalCbFnT>::originalCallback
			.callbackFn)
		{
			caller->getIoService().post(
				STC(std::bind(
					AsynchronousContinuation<OriginalCbFnT>::originalCallback
						.callbackFn,
					std::forward<Args>(args)...)));
		}
	}

public:
	std::shared_ptr<ComponentThread> caller;
};

} // namespace sscl

#endif // ASYNCHRONOUS_CONTINUATION_H
