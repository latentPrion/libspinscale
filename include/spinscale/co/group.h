#ifndef GROUP_H
#define GROUP_H

#include <any>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <spinscale/componentThread.h>
#include <spinscale/co/promiseChainLink.h>
#include <spinscale/sharedResourceGroup.h>
#include <spinscale/spinLock.h>

namespace sscl::co {

namespace detail {

template <typename T, typename H>
concept await_suspend_returns_void = requires(T &t, H h) {
	{ t.await_suspend(h) } -> std::same_as<void>;
};

template <typename T, typename H>
concept await_suspend_returns_bool = requires(T &t, H h) {
	{ t.await_suspend(h) } -> std::convertible_to<bool>;
};

template <typename T, typename H>
concept await_suspend_returns_handle = requires(T &t, H h) {
	{ t.await_suspend(h) } -> std::convertible_to<std::coroutine_handle<>>;
};

template <typename T, typename H>
concept await_suspend_ok = await_suspend_returns_void<T, H>
	|| await_suspend_returns_bool<T, H>
	|| await_suspend_returns_handle<T, H>;

template <typename T, typename H = std::coroutine_handle<>>
concept AwaiterIface = requires(T &t, H h) {
	{ t.await_ready() } -> std::convertible_to<bool>;
	{ t.await_resume() };
} && await_suspend_ok<T, H>;

template <typename T>
auto get_operator_co_await(T &t) -> decltype(operator co_await(t));

template <typename T>
concept AwaitableIface = requires(T &t) {
	{ get_operator_co_await(t) };
} && AwaiterIface<decltype(get_operator_co_await(std::declval<T &>()))>;

template<AwaiterIface T>
T &asAwaiter(T &t) noexcept
{
	return t;
}

template<AwaitableIface T>
auto asAwaiter(T &t) noexcept(noexcept(get_operator_co_await(t)))
	-> decltype(get_operator_co_await(t))
{
	return get_operator_co_await(t);
}

} // namespace detail

template <typename T>
concept AwaitableIface = detail::AwaitableIface<T>;

template <typename T>
concept AwaiterIface = detail::AwaiterIface<T>;

template <typename T>
concept AwaitableOrAwaiterIface = AwaiterIface<T> || AwaitableIface<T>;

/** Typical usage — parallel members, then gather:
 *
 *   co::Group group;
 *
 *   auto bodyInit = body.initializeCReq(exceptionPtr, noopCallback);
 *   auto legInit  = leg.initializeCReq(exceptionPtr, noopCallback);
 *   ViralNonPostingInvoker<void> batch = app.joltAllPuppetThreadsCReq(...);
 *
 *   group.add(bodyInit);
 *   group.add(legInit);
 *   group.add(batch);
 *
 *   co_await group.getAwaitAllSettlementsInvoker();
 *   group.checkForAndReThrowGroupExceptions();
 *
 *   (void)bodyInit.completedReturnValues();
 *
 *   // When walking settlement slots by index:
 *   settlements[i].invokerAs<BodyViralPostingInvoker<void>>()
 *       .completedReturnValues();
 */
struct Group
{
	enum class AwaitingCondition {
		NONE, FIRST_SETTLED, ALL_SETTLED
	};

	class SettlementDescriptor
	{
	public:
		enum class TypeE {
			/* We track EXCEPTIION_THROWN but we don't provide an
			 * awaitInvoker for exception events. The caller can
			 * wait for settlements and then scan the result set
			 * to manually deal with exceptions.
			 */
			UNSETTLED, COMPLETED, EXCEPTION_THROWN
		};

		template<typename Member>
		void bindMemberRef(Member &member)
		{
			memberInvokerRef = std::ref(member);
		}

		template<typename Member>
		Member &invokerAs() const
		{
			try {
				return std::any_cast<std::reference_wrapper<Member>>(
					memberInvokerRef).get();
			} catch (const std::bad_any_cast &) {
				throw std::runtime_error(
					"Group settlement invoker type mismatch");
			}
		}

		void setSettlementStatus() noexcept
		{
			assert(type == TypeE::UNSETTLED);

			if (calleeException) {
				type = TypeE::EXCEPTION_THROWN;
			} else {
				type = TypeE::COMPLETED;
			}
		}

		TypeE type = TypeE::UNSETTLED;
		std::exception_ptr calleeException = nullptr;
		std::exception_ptr adapterException = nullptr;
		std::any memberInvokerRef;
	};

	struct SettlementAwaitingInvoker;
	struct AwaitFirstSettlementInvoker;
	struct AwaitAllSettlementsInvoker;

	// getAwaitNextSettlementInvoker();
	AwaitFirstSettlementInvoker getAwaitFirstSettlementInvoker()
		{ return AwaitFirstSettlementInvoker(*this); }

	AwaitAllSettlementsInvoker getAwaitAllSettlementsInvoker()
		{ return AwaitAllSettlementsInvoker(*this); }

	bool verifyAllInvokersSettled() const
	{
		for (auto &desc : s.rsrc.settlements) {
			if (desc.type == SettlementDescriptor::TypeE::UNSETTLED) {
				return false;
			}
		}

		return true;
	}

	bool firstInvokerSettled() const
		{ return s.rsrc.firstSettledInvokerIdx >= 0; }

	bool allInvokersSettled() const
	{
		const std::size_t nInvokersAdded = s.rsrc.settlements.size();
		assert(s.rsrc.nInvokersSettled <= nInvokersAdded);
		return s.rsrc.nInvokersSettled == nInvokersAdded;
	}

	/**	Caller must hold s.lock. */
	void throwIfNoMemberInvokersForCoAwaitUnderLock() const
	{
		if (s.rsrc.settlements.empty()) {
			throw std::runtime_error(
				"co_await: Group has no member invokers; call add() before awaiting");
		}
	}

	struct SettlementAwaitingInvoker
	{
		explicit SettlementAwaitingInvoker(Group &_group)
		: parentGroup(_group)
		{}

		bool await_ready() const { return false; }

		/**	EXPLANATION:
		 * This exists for if we ever need to re-make the adapter coro
		 * throw exceptions. But we decided to make it noexcept in order
		 * to avoid this complication.
		 */
		void checkForAndReThrowAdapterExceptions() const
		{
			std::ostringstream ostream;
			bool doThrow = false;

			for (auto &item : parentGroup.s.rsrc.settlements)
			{
				if (!item.adapterException) {
					continue;
				}

				doThrow = true;
				ostream << "Exc thrown in Group Adapter: ";
				try {
					std::rethrow_exception(item.adapterException);
				} catch (const std::exception &e) {
					ostream << e.what();
				} catch (...) {
					ostream << "<unknown exception type>";
				}
				ostream << "\n";
			}

			if (doThrow) {
				throw std::runtime_error(ostream.str());
			}
		}

		Group &parentGroup;
	};

	/**	EXPLANATION:
	 * AwaitingCondition and the group-awaiter coroutine_handle are set only
	 * in await_suspend when this co_await actually suspends. Constructing
	 * several AwaitFirstSettlementInvoker / AwaitAllSettlementsInvoker
	 * objects without co_awaiting them is harmless.
	 *
	 * You may co_await await-all and later co_await await-first (in either
	 * construction order). After a suspending wait completes, the adapter
	 * clears handle state in updateSettlementsStateAndAwakenCallerIfConditionMet,
	 * so a later co_await on another handle (or a second co_await on the same
	 * handle, after the first finished) is legal.
	 *
	 * Only one group co_await may be suspended with a registered handle at a
	 * time; a second concurrent co_await trips assert(!callerHasSetSchedHandle)
	 * in debug builds.
	 *
	 * firstSettledInvokerIdx and calleeWasReadyToNotifyOfFirstSettlement are
	 * sticky for the Group lifetime (first member ever to settle), not per wave.
	 */
	struct AwaitFirstSettlementInvoker
	:	public SettlementAwaitingInvoker
	{
		using SettlementAwaitingInvoker::SettlementAwaitingInvoker;

		bool await_suspend(std::coroutine_handle<> groupAwaiterSchedHandle)
		{
			/* No other group co_await may be suspended with a registered handle.
			 * Sequential co_await on the same object is allowed after the prior
			 * wait finished and clearCallerSchedHandleState() ran on wake.
			 */
			assert(!this->parentGroup.s.rsrc.callerHasSetSchedHandle);

			sscl::SpinLock::Guard guard(this->parentGroup.s.lock);

			this->parentGroup.throwIfNoMemberInvokersForCoAwaitUnderLock();

			if (this->parentGroup.s.rsrc.calleeWasReadyToNotifyOfFirstSettlement) {
				return false;
			}

			/* We store away the coro_handle of the
			 * group awaiter, and suspend that group awaiter.
			 */
			this->parentGroup.s.rsrc.setCallerSchedHandleAndCondition(
				groupAwaiterSchedHandle, AwaitingCondition::FIRST_SETTLED);

			return true;
		}

		std::pair<SettlementDescriptor &, std::vector<SettlementDescriptor> &>
		await_resume()
		{
			assert(this->parentGroup.firstInvokerSettled());
			return {
				this->parentGroup.s.rsrc.settlements[
					this->parentGroup.s.rsrc.firstSettledInvokerIdx],
				this->parentGroup.s.rsrc.settlements
			};
		}
	};

	/**	EXPLANATION:
	 * Same awaiting rules as AwaitFirstSettlementInvoker (see above).
	 *
	 * It is illegal to add() new members while a group co_await is suspended
	 * (groupAwaiterSchedHandle is registered). You may add() after co_await
	 * returns, including starting a new settlement wave before the next
	 * co_await.
	 */
	struct AwaitAllSettlementsInvoker
	:	public SettlementAwaitingInvoker
	{
		using SettlementAwaitingInvoker::SettlementAwaitingInvoker;

		bool await_suspend(std::coroutine_handle<> groupAwaiterSchedHandle)
		{
			/* See AwaitFirstSettlementInvoker::await_suspend. Handle state is
			 * cleared when the adapter wakes a suspended group co_awaiter, not
			 * in await_resume.
			 */
			assert(!this->parentGroup.s.rsrc.callerHasSetSchedHandle);

			sscl::SpinLock::Guard guard(this->parentGroup.s.lock);

			this->parentGroup.throwIfNoMemberInvokersForCoAwaitUnderLock();

			if (this->parentGroup.allInvokersSettled()) {
				return false;
			}

			this->parentGroup.s.rsrc.setCallerSchedHandleAndCondition(
				groupAwaiterSchedHandle, AwaitingCondition::ALL_SETTLED);

			return true;
		}

		std::vector<SettlementDescriptor> &await_resume()
		{
			assert(this->parentGroup.allInvokersSettled());
			return this->parentGroup.s.rsrc.settlements;
		}
	};

	struct NonAwaitableNonPostingAdapterCoro
	{
		struct promise_type
		:	public PromiseChainLink
		{
			NonAwaitableNonPostingAdapterCoro get_return_object() noexcept
				{ return {}; }

			void removeAcquiredLock(CoQutex &) noexcept override
				{}

			std::suspend_never initial_suspend() noexcept { return {}; }
			/**	EXPLANATION:
			 * final_suspend must return suspend_never here so that
			 * this fire-and-forget adapter coro will be self-destroying.
			 */
			std::suspend_never final_suspend() noexcept { return {}; }
			void return_void() noexcept { return; }
			void unhandled_exception() noexcept
			{
				try {
					auto eptr = std::current_exception();
					if (eptr) {
						std::rethrow_exception(eptr);
					}
				} catch (const std::exception &e) {
					std::cerr << "Unhandled exception in Group adapter coroutine:\n"
						<< e.what() << "\n";
				} catch (...) {
					std::cerr << "Unhandled non-std exception in Group adapter coroutine\n";
				}

				std::terminate();
			}
		};

		NonAwaitableNonPostingAdapterCoro() noexcept = default;
		NonAwaitableNonPostingAdapterCoro operator co_await() const = delete;
		bool await_ready() const { std::terminate(); return false; }
		void await_suspend() const { std::terminate(); }
		void await_resume() const { std::terminate(); }
	};

	std::pair<bool, bool>
	updateSettlementsStateAndAwakenCallerIfConditionMet(
		std::size_t settlementIndex) noexcept
	{
		bool isFirstSettlement = false;
		bool isLastSettlement = false;
		std::coroutine_handle<> groupAwaiterSchedHandleToWake = nullptr;

		{
			sscl::SpinLock::Guard guard(s.lock);

			/* If we can be certain that the AllSettled condition won't
			 * be triggered repeatedly, then we can get rid of
			 * calleeWasReadyToNotifyOfLastSettlementForCurrentSet.
			 */
			assert(s.rsrc.nInvokersSettled < s.rsrc.settlements.size());
			assert(settlementIndex < s.rsrc.settlements.size());
			s.rsrc.nInvokersSettled++;

			if (!firstInvokerSettled())
			{
				isFirstSettlement = true;
				s.rsrc.firstSettledInvokerIdx = static_cast<int>(settlementIndex);

				/* This should be set-once & sticky throughout the lifetime
				 * of the Group object. The first invoker only gets
				 * settled once, irrespective of how many
				 * AwaitFirstSettlementInvoker instances we create.
				 */
				s.rsrc.calleeWasReadyToNotifyOfFirstSettlement = true;
			}

			if (allInvokersSettled())
			{
				assert(s.rsrc.nInvokersSettled == s.rsrc.settlements.size());
				assert(verifyAllInvokersSettled());
				isLastSettlement = true;
			}

			/* If no group co_awaiter registered a handle (did not suspend, or
			 * already woke and clearCallerSchedHandleState ran), there is
			 * nothing to post back to.
			 */
			if (!s.rsrc.callerHasSetSchedHandle) {
				return {isFirstSettlement, isLastSettlement};
			}

			/* If we're here, then callerHasSetSchedHandle must be true.
			 * I.e: an invoker has been created and co_awaited for one of the
			 * conditions.
			 * Therefore currentAwaitingCondition must also have been set,
			 * since currentAwaitingCondition is set in the invokers' ctors.
			 */
			assert(s.rsrc.currentAwaitingCondition != AwaitingCondition::NONE);

			if ((isFirstSettlement
					&& s.rsrc.currentAwaitingCondition == AwaitingCondition::FIRST_SETTLED)
				|| (isLastSettlement
					&& s.rsrc.currentAwaitingCondition == AwaitingCondition::ALL_SETTLED))
			{
				groupAwaiterSchedHandleToWake = s.rsrc.groupAwaiterSchedHandle;

				/** We only clear here and not in await_resume, because if
				 * the caller hasn't already set it schedHandle by the time we're
				 * called, then when it eventually does call await_suspend, it
				 * won't set it then either.
				 *
				 * I.e: callerSchedHandle only needs to be cleared it if gets set
				 * in the first place;
				 * And it only gets set if we need to invoke the schedHandle from
				 * here.
				 * If the group co_awaiter is able to call await_resume, then it
				 * simply doesn't set its schedHandle at all.
				 */
				s.rsrc.clearCallerSchedHandleState();
			}
		}

		if (groupAwaiterSchedHandleToWake)
		{
			/* We should be able to just directly resume() the group awaiter's handle
			 * here because that would invoke await_resume, which may destroy the
			 * callee's promise.
			 * And who is the callee? Is it not this coro here? And this coro
			 * hasn't been suspended. So we'd be destroying ourself while we're
			 * not suspended.
			 *
			 * But all of that only applies __IFF__ we actually do try to destroy
			 * the callee within the caller's Invoker. If we don't, then the callee
			 * should persist just fine. There's no implicit mechanism that
			 * will always destroy the callee coro state before the invoker
			 * is destroyed.
			 * If that was in fact the way it worked, then fire-and-forget coros
			 * would be impossible.
			 *
			 * So we should be able to call resume() directly here without
			 * post()ing to ComponentThread::getSelf()->getIoContext().
			 *
			 *	EXPLANATION:
			 * However, in order to ensure that we keep this adapter coro
			 * method exception-free, we are forced to post() rather than
			 * directly calling the handle.
			 */
			boost::asio::post(
				sscl::ComponentThread::getSelf()->getIoContext(),
				groupAwaiterSchedHandleToWake);
		}

		return {isFirstSettlement, isLastSettlement};
	}

	/**	EXPLANATION:
	 * This coro is a coro which has a promise, and does __not__ expose an awaitable
	 * iface and in fact should not be capable of being awaited, ultimately.
	 *
	 * Its purpose is to be an adapter that enables the Group class to invoke the
	 * invokers that are added to it, without having to co_await those invokers.
	 * Rather, the Group class simply invokes this function on them, and then this
	 * function both co_awaits the invoker on behalf of the Group class, and also
	 * performs the normal function of an invoker, which is both to invoke the
	 * target async fn, and also to convey its results back to the Group class.
	 * It's effectively a go-between coro that provides the outcomes that Invokers
	 * normally provide, without needing, itself, to be co_awaited.
	 *
	 * settlementIndex is captured by value (not a vector iterator) so adapter
	 * coros remain valid if settlements reallocate during concurrent add().
	 */
	template<AwaitableOrAwaiterIface Member>
	NonAwaitableNonPostingAdapterCoro memberAdapterCoro(
		Member &memberInvoker,
		std::size_t settlementIndex) noexcept
	{
		try {
			co_await detail::asAwaiter(memberInvoker);
		}
		catch (...)
		{
			s.rsrc.settlements[settlementIndex].calleeException =
				std::current_exception();
		}

		/* From here onwards, we mustn't throw(). Unhandled exceptions
		 * generated by the adapter coro itself will result in
		 * std::terminate().
		 */
		s.rsrc.settlements[settlementIndex].setSettlementStatus();
		updateSettlementsStateAndAwakenCallerIfConditionMet(settlementIndex);

		co_return;
	}

	template<AwaitableOrAwaiterIface Member>
	void add(Member &memberInvoker)
	{
		std::size_t settlementIndex = 0;

		{
			sscl::SpinLock::Guard guard(s.lock);

			if (s.rsrc.groupAwaiterSchedHandle)
			{
				throw std::runtime_error(
					"add: New member invokers mustn't be added "
					"while co_awaiting a given set");
			}

			settlementIndex = s.rsrc.settlements.size();
			s.rsrc.settlements.emplace_back();
			s.rsrc.settlements[settlementIndex].bindMemberRef(memberInvoker);
		}

		memberAdapterCoro(memberInvoker, settlementIndex);
	}

	std::exception_ptr captureAggregatedGroupExceptions() const
	{
		std::ostringstream ostream;
		bool hasFailures = false;

		for (auto &item : s.rsrc.settlements)
		{
			if (item.type != SettlementDescriptor::TypeE::EXCEPTION_THROWN) {
				continue;
			}

			assert(item.calleeException);

			hasFailures = true;
			ostream << "Exc thrown in Group Adapter: ";
			try {
				std::rethrow_exception(item.calleeException);
			} catch (const std::exception &e) {
				ostream << e.what();
			} catch (...) {
				ostream << "<unknown exception type>";
			}
			ostream << "\n";
		}

		if (!hasFailures) {
			return nullptr;
		}

		return std::make_exception_ptr(std::runtime_error(ostream.str()));
	}

	void checkForAndReThrowGroupExceptions() const
	{
		std::exception_ptr aggregatedException =
			captureAggregatedGroupExceptions();
		if (aggregatedException) {
			std::rethrow_exception(aggregatedException);
		}
	}

	struct State
	{
		void clearCallerSchedHandleState() noexcept
		{
			groupAwaiterSchedHandle = nullptr;
			callerHasSetSchedHandle = false;
			currentAwaitingCondition = AwaitingCondition::NONE;
		}

		void setCallerSchedHandleAndCondition(
			std::coroutine_handle<> groupAwaiterSchedHandleIn,
			AwaitingCondition awaitingCondition) noexcept
		{
			groupAwaiterSchedHandle = groupAwaiterSchedHandleIn;
			callerHasSetSchedHandle = true;
			currentAwaitingCondition = awaitingCondition;
		}

		int firstSettledInvokerIdx = -1;
		std::size_t nInvokersSettled = 0;
		std::coroutine_handle<> groupAwaiterSchedHandle = nullptr;
		bool callerHasSetSchedHandle = false;
		/* calleWasReady*First* is an indelible record of what
		 * occured during the first settlement's adapter's update.
		 */
		bool calleeWasReadyToNotifyOfFirstSettlement = false;
		std::vector<SettlementDescriptor> settlements;
		AwaitingCondition currentAwaitingCondition = AwaitingCondition::NONE;
	};

	sscl::SharedResourceGroup<sscl::SpinLock, State> s;
};

} // namespace sscl::co

#endif // GROUP_H
