#ifndef CO_CONDITION_VARIABLE_H
#define CO_CONDITION_VARIABLE_H

#include <config.h>
#include <coroutine>
#include <deque>
#include <iostream>
#include <memory>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <spinscale/componentThread.h>
#include <spinscale/spinLock.h>

namespace sscl::co {

/**	Coroutine-friendly handoff: wait until `signal()` before running a completion
 *	step. Standalone primitive (posting promises use `PostBackStatus` instead).
 *
 *	`clear()` only clears `isSignaled`; it does not wake or drain waiters. If
 *	`clear()` runs while coroutines are still waiting, they stay queued until a
 *	later `signal()` posts them.
 */
class CoConditionVariable
{
public:
	/**	Waiter queued under the CV spin lock; `signal()` drains and calls `post()`. */
	class WaitingCoroutineBase
	{
	public:
		explicit WaitingCoroutineBase(
			boost::asio::io_context &callerIoContextIn) noexcept
		: callerIoContext(callerIoContextIn)
		{}

		virtual ~WaitingCoroutineBase() = default;

		virtual void post() noexcept = 0;

	public:
		boost::asio::io_context &callerIoContext;
	};

	template <typename Promise>
	class TypedWaitingCoroutine
	:	public WaitingCoroutineBase
	{
	public:
		TypedWaitingCoroutine(
			boost::asio::io_context &callerIoContextIn,
			std::coroutine_handle<Promise> callerSchedHandleIn) noexcept
		: WaitingCoroutineBase(callerIoContextIn),
		  callerSchedHandle(callerSchedHandleIn)
		{}

		void post() noexcept override
		{
			boost::asio::post(callerIoContext, callerSchedHandle);
		}

	public:
		std::coroutine_handle<Promise> callerSchedHandle;
	};

	struct OperationInvoker
	{
		explicit OperationInvoker(CoConditionVariable &parentCvIn) noexcept
		: parentCv(parentCvIn)
		{}

		CoConditionVariable &parentCv;
	};

	struct WaitForInvoker
	:	public OperationInvoker
	{
		using OperationInvoker::OperationInvoker;

		bool await_ready() const noexcept { return false; }

		template <typename Promise>
		bool await_suspend(std::coroutine_handle<Promise> cvCallerSchedHandle) noexcept
		{
			boost::asio::io_context &cvCallerIoContext =
				sscl::ComponentThread::getSelf()->getIoContext();

			sscl::SpinLock::Guard guard(parentCv.spinLock);
			if (parentCv.isSignaled) {
				return false;
			}

#ifdef CONFIG_LIBSSCL_DEBUG_CO
			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " CV not signaled: Enqueuing waiter coroutine.\n";
#endif
			parentCv.enqueueWaitingCoroutine(
				cvCallerSchedHandle, cvCallerIoContext);

			return true;
		}

		void await_resume() const noexcept {}
	};

	/**	Manual await-style API only (lowerCamelCase); not a coroutine awaiter.
	 *	`FinalSuspendPostingInvoker` calls `awaitSuspend` explicitly.
	 */
	struct DecisionEnablingDerivableWaitForInvoker
	:	public OperationInvoker
	{
		struct DecisionFactors
		{
			sscl::SpinLock &cvInternalSpinLock;
			bool wasAlreadySignaled;

			DecisionFactors(sscl::SpinLock &cvLockIn, bool signaledIn) noexcept
			: cvInternalSpinLock(cvLockIn),
			  wasAlreadySignaled(signaledIn)
			{}
		};

		using OperationInvoker::OperationInvoker;

		void operator co_await() const = delete;

		bool awaitReady() const noexcept { return false; }

		template <typename Promise>
		DecisionFactors awaitSuspend(std::coroutine_handle<Promise> cvCallerSchedHandle) noexcept
		{
			boost::asio::io_context &cvCallerIoContext =
				sscl::ComponentThread::getSelf()->getIoContext();

			parentCv.spinLock.acquire();
			if (parentCv.isSignaled)
			{
#ifdef CONFIG_LIBSSCL_DEBUG_CO
				std::cout << __func__ << ": " << std::this_thread::get_id()
					<< " CV already signaled: returning already-signaled DecisionFactors.\n";
#endif
				return DecisionFactors(parentCv.spinLock, true);
			}

#ifdef CONFIG_LIBSSCL_DEBUG_CO
			std::cout << __func__ << ": " << std::this_thread::get_id()
				<< " CV not signaled: returning not-signaled DecisionFactors.\n";
#endif
			parentCv.enqueueWaitingCoroutine(
				cvCallerSchedHandle, cvCallerIoContext);

			return DecisionFactors(parentCv.spinLock, false);
		}

		void awaitResume() const noexcept {}
	};

	CoConditionVariable() noexcept = default;
	CoConditionVariable(const CoConditionVariable &) = delete;
	CoConditionVariable &operator=(const CoConditionVariable &) = delete;
	CoConditionVariable(CoConditionVariable &&) noexcept = delete;
	CoConditionVariable &operator=(CoConditionVariable &&) noexcept = delete;
	~CoConditionVariable() noexcept = default;

	WaitForInvoker getWaitForInvoker() noexcept
		{ return WaitForInvoker(*this); }

	DecisionEnablingDerivableWaitForInvoker
	getDecisionEnablingDerivableWaitForInvoker() noexcept
	{
		return DecisionEnablingDerivableWaitForInvoker(*this);
	}

	void signal() noexcept
	{
		std::deque<std::unique_ptr<WaitingCoroutineBase>> drained;

		{
			sscl::SpinLock::Guard guard(spinLock);
			isSignaled = true;
			drained.swap(waitingCoroutines);
		}

		for (std::unique_ptr<WaitingCoroutineBase> &waiter : drained) {
			waiter->post();
		}
	}

	/**	Only clears the signaled flag; waiters (if any) remain in the deque. */
	void clear() noexcept
	{
		sscl::SpinLock::Guard guard(spinLock);
		isSignaled = false;
	}

	template <typename Promise>
	void enqueueWaitingCoroutine(
		std::coroutine_handle<Promise> handle,
		boost::asio::io_context &ctx) noexcept
	{
		waitingCoroutines.push_back(
			std::make_unique<TypedWaitingCoroutine<Promise>>(ctx, handle));
	}

private:
	sscl::SpinLock spinLock;
	bool isSignaled = false;
	std::deque<std::unique_ptr<WaitingCoroutineBase>> waitingCoroutines;
};

} // namespace sscl::co

#endif // CO_CONDITION_VARIABLE_H
