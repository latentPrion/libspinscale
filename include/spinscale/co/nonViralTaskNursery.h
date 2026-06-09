#ifndef NON_VIRAL_TASK_NURSERY_H
#define NON_VIRAL_TASK_NURSERY_H

#include <cstddef>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/asio/io_context.hpp>

#include <spinscale/cps/asynchronousBridge.h>
#include <spinscale/sharedResourceGroup.h>
#include <spinscale/spinLock.h>
#include <spinscale/syncCancelerForAsyncWork.h>

namespace sscl::co {

namespace detail {

struct MemberInvokerBase
{
	virtual ~MemberInvokerBase() = default;
};

template <class Invoker>
struct MemberInvoker : MemberInvokerBase
{
	explicit MemberInvoker(Invoker &&invokerIn)
	: invoker(std::move(invokerIn))
	{}

	Invoker invoker;
};

} // namespace detail

/**	Structured-concurrency owner for non-viral invokers at non-coroutine boundaries.
 *
 *	The nursery owns invoker lifetimes until natural completion, wraps completion
 *	callbacks, tracks unsettled slots, fans out cooperative cancel via per-slot
 *	SyncCancelerForAsyncWork, and provides drain APIs.
 *
 *	Call closeAdmission() explicitly before asyncAwaitAllSettlements() or
 *	syncAwaitAllSettlements().
 *
 *	syncAwaitAllSettlements() runs a nested io_context loop on the calling
 *	thread (AsynchronousBridge). Pass the calling thread's io_context —
 *	typically
 *	ComponentThread::getSelf()->getIoContext() — not another thread's
 *	io_context. If the caller pumps a different thread's queue while blocked,
 *	completions posted back to the caller's own io_context are never executed
 *	and the drain can deadlock even after cooperative cancel.
 */
class NonViralTaskNursery
{
public:
	enum class SlotLeaseStatus {
		RESERVED, ACTIVE_UNSETTLED, RETIRED
	};

	enum class SlotSettlementStatus {
		UNSETTLED, COMPLETED, EXCEPTION_THROWN
	};

	class Slot
	{
	public:
		/** Opaque handle to a nursery slot. Valid while the slot remains in storage. */
		class Handle
		{
		public:
			bool operator==(const Handle &_other) const noexcept
				{ return &slot == &_other.slot; }

			bool operator!=(const Handle &_other) const noexcept
				{ return &slot != &_other.slot; }

		private:
			friend class NonViralTaskNursery;
			friend class Lease;

			explicit Handle(Slot &_slot) noexcept
			: slot(_slot)
			{}

			Slot &slot;
		};

		class Lease
		{
		public:
			Lease(Lease &&_other) noexcept
			: nursery(_other.nursery), slot(_other.slot),
			slotCommittedSoLeaseShouldntDestroy(
				std::exchange(
					_other.slotCommittedSoLeaseShouldntDestroy,
					true))
			{}

			Lease(const Lease &) = delete;
			Lease &operator=(const Lease &) = delete;
			Lease &operator=(Lease &&) = delete;

			~Lease()
			{
				if (!slotCommittedSoLeaseShouldntDestroy) {
					nursery.releaseUncommittedSlot(slot);
				}
			}

			std::exception_ptr &getExceptionStorage()
				{ return slot.exceptionPtr; }

			std::function<void()> getCallerLambda()
				{ return nursery.buildCallerLambdaForSlot(slot); }

			sscl::SyncCancelerForAsyncWork &getSyncCanceler()
				{ return slot.syncCanceler; }

			void setOnSettledHook(
				std::function<void(std::exception_ptr &exceptionPtr)> hook)
			{
				if (slot.leaseStatus != SlotLeaseStatus::RESERVED)
				{
					throw std::runtime_error(
						std::string(__func__)
						+ ": must be called before fillSlot()");
				}

				slot.onSettledHook = std::move(hook);
			}

			/**	Factory must create the invoker. Deferred construction is
			 * required because non-viral coroutines may complete synchronously
			 * during invoker construction, before fillSlot() can store the
			 * record.
			 */
			template <class InvokerFactory>
			void fillSlot(InvokerFactory &&invokerFactory)
			{
				Slot &reservedSlot = slot;

				if (reservedSlot.memberInvoker)
				{
					throw std::runtime_error(
						std::string(__func__) + ": slot already filled");
				}

				if (reservedSlot.leaseStatus != SlotLeaseStatus::RESERVED)
				{
					throw std::runtime_error(
						std::string(__func__) + ": slot is not reserved");
				}

				reservedSlot.leaseStatus = SlotLeaseStatus::ACTIVE_UNSETTLED;
				auto invoker = invokerFactory();

				if (reservedSlot.leaseStatus == SlotLeaseStatus::RETIRED)
				{
					/**	EXPLANATION:
					 * Non-viral coroutines may complete synchronously inside
					 * the factory. Retirement already ran; the local invoker
					 * must be allowed to destroy the callee frame on return.
					 */
					return;
				}

				reservedSlot.memberInvoker = std::make_unique<
					detail::MemberInvoker<std::decay_t<decltype(invoker)>>>(
					std::move(invoker));
			}

			void commit()
			{
				if (slotCommittedSoLeaseShouldntDestroy)
				{
					throw std::runtime_error(
						std::string(__func__) + ": lease already committed");
				}

				Slot &reservedSlot = slot;

				if (reservedSlot.leaseStatus == SlotLeaseStatus::RESERVED)
				{
					throw std::runtime_error(
						std::string(__func__)
						+ ": fillSlot() required before commit()");
				}

				slotCommittedSoLeaseShouldntDestroy = true;
			}

			Handle handle() const
			{
				return Handle(slot);
			}

		private:
			friend class NonViralTaskNursery;

			Lease(NonViralTaskNursery &_nursery, Slot &_slot) noexcept
			: nursery(_nursery), slot(_slot)
			{}

			NonViralTaskNursery &nursery;
			Slot &slot;
			bool slotCommittedSoLeaseShouldntDestroy = false;
		};

	private:
		friend class NonViralTaskNursery;
		friend class Lease;

		SlotLeaseStatus leaseStatus = SlotLeaseStatus::RESERVED;
		SlotSettlementStatus settlementStatus = SlotSettlementStatus::UNSETTLED;
		std::exception_ptr exceptionPtr = nullptr;
		sscl::SyncCancelerForAsyncWork syncCanceler;
		std::unique_ptr<detail::MemberInvokerBase> memberInvoker;
		std::function<void(std::exception_ptr &exceptionPtr)> onSettledHook;
	};

	void openAdmission()
	{
		sscl::SpinLock::Guard guard(s.lock);
		s.rsrc.admissionOpen = true;
	}

	void closeAdmission()
	{
		sscl::SpinLock::Guard guard(s.lock);
		s.rsrc.admissionOpen = false;
	}

	bool admissionIsOpen() const
	{
		sscl::SpinLock::Guard guard(s.lock);
		return s.rsrc.admissionOpen;
	}

	bool allSettled() const
		{ return unsettledCount() == 0; }

	std::size_t unsettledCount() const
	{
		sscl::SpinLock::Guard guard(s.lock);
		return countUnsettledSlotsUnlocked();
	}

	Slot::Lease getNewSlotLease()
	{
		sscl::SpinLock::Guard guard(s.lock);

		if (!s.rsrc.admissionOpen)
		{
			throw std::runtime_error(
				std::string(__func__) + ": admission closed");
		}

		pruneRetiredSlotsUnlocked();
		s.rsrc.slots.emplace_back();
		Slot &slot = s.rsrc.slots.back();
		return Slot::Lease(*this, slot);
	}

	void requestCancelOnAll()
	{
		sscl::SpinLock::Guard guard(s.lock);

		for (auto &slot : s.rsrc.slots)
		{
			if (slot.leaseStatus != SlotLeaseStatus::ACTIVE_UNSETTLED) {
				continue;
			}

			slot.syncCanceler.requestStop();
		}
	}

	void asyncAwaitAllSettlements(std::function<void()> callback)
	{
		std::function<void()> waiterToInvoke;

		{
			sscl::SpinLock::Guard guard(s.lock);

			if (s.rsrc.admissionOpen)
			{
				throw std::runtime_error(
					std::string(__func__)
					+ ": admission must be closed before awaiting drain");
			}

			if (countUnsettledSlotsUnlocked() == 0) {
				waiterToInvoke = std::move(callback);
			}
			else if (s.rsrc.drainWaiter)
			{
				throw std::runtime_error(
					std::string(__func__)
					+ ": drain waiter already registered");
			}
			else {
				s.rsrc.drainWaiter = std::move(callback);
			}
		}

		if (waiterToInvoke) {
			waiterToInvoke();
		}
	}

	/** Nested drain: blocks the calling thread in run_one() on @p ioContext until
	 * all slots retire. @p ioContext must be the caller thread's io_context.
	 */
	void syncAwaitAllSettlements(boost::asio::io_context &ioContext)
	{
		if (admissionIsOpen())
		{
			throw std::runtime_error(
				std::string(__func__)
				+ ": admission must be closed before awaiting drain");
		}

		if (allSettled()) {
			return;
		}

		if (ioContext.stopped())
		{
			throw std::runtime_error(
				std::string(__func__) + ": provided io_context is stopped");
		}

		/**	EXPLANATION:
		 * Drain may run on the thread that processes a completion callback,
		 * not necessarily the thread blocked in waitForAsyncOperationComplete.
		 * Keep the bridge off the waiter thread's stack.
		 */
		auto bridge = std::make_shared<sscl::cps::AsynchronousBridge>(
			ioContext);
		asyncAwaitAllSettlements(
			[bridge]()
			{
				bridge->setAsyncOperationComplete();
			});

		bridge->waitForAsyncOperationCompleteOrIoContextStopped();
	}

	template <class InvokerFactory>
	Slot::Handle launch(
		InvokerFactory &&factory,
		std::function<void(std::exception_ptr &exceptionPtr)> onSettledHook =
			nullptr)
	{
		auto lease = getNewSlotLease();
		lease.getSyncCanceler().startAcceptingWork();
		if (onSettledHook) {
			lease.setOnSettledHook(std::move(onSettledHook));
		}
		lease.fillSlot(
			[&factory, &lease]()
			{
				return std::forward<InvokerFactory>(factory)(lease);
			});
		lease.commit();
		return lease.handle();
	}

private:
	friend class Slot::Lease;

	struct State
	{
		bool admissionOpen = false;
		std::list<Slot> slots;
		std::function<void()> drainWaiter;
	};

	std::size_t countUnsettledSlotsUnlocked() const
	{
		std::size_t count = 0;

		for (const auto &slot : s.rsrc.slots)
		{
			if (slot.leaseStatus == SlotLeaseStatus::ACTIVE_UNSETTLED) {
				++count;
			}
		}

		return count;
	}

	void releaseUncommittedSlot(Slot &slot)
	{
		std::function<void()> waiterToInvoke;

		{
			sscl::SpinLock::Guard guard(s.lock);

			if (slot.leaseStatus != SlotLeaseStatus::RESERVED) {
				return;
			}

			slot.leaseStatus = SlotLeaseStatus::RETIRED;
			slot.memberInvoker.reset();
			waiterToInvoke = takeDrainWaiterIfDrainedUnlocked();
		}

		if (waiterToInvoke) { waiterToInvoke(); }
	}

	std::function<void()> buildCallerLambdaForSlot(Slot &slot)
	{
		return
			[this, slot = std::ref(slot)]()
			{
				retireSlot(slot.get());
			};
	}

	void retireSlot(Slot &slot)
	{
		std::function<void()> waiterToInvoke;
		std::function<void(std::exception_ptr &exceptionPtr)> onSettledHook;
		std::exception_ptr settledExceptionPtr;

		{
			sscl::SpinLock::Guard guard(s.lock);

			if (slot.leaseStatus != SlotLeaseStatus::ACTIVE_UNSETTLED)
			{
				throw std::runtime_error(
					std::string(__func__) + ": slot is not active and "
					"unsettled");
			}
			if (slot.settlementStatus != SlotSettlementStatus::UNSETTLED) {
				throw std::runtime_error(
					std::string(__func__) + ": slot is not unsettled");
			}

			if (!verifySlotIsManagedUnlocked(slot)) {
				return;
			}

			settledExceptionPtr = slot.exceptionPtr;
			if (settledExceptionPtr) {
				slot.settlementStatus = SlotSettlementStatus::EXCEPTION_THROWN;
			} else {
				slot.settlementStatus = SlotSettlementStatus::COMPLETED;
			}

			onSettledHook = std::move(slot.onSettledHook);
			slot.leaseStatus = SlotLeaseStatus::RETIRED;
			slot.memberInvoker.reset();
			waiterToInvoke = takeDrainWaiterIfDrainedUnlocked();
		}

		if (onSettledHook) {
			onSettledHook(settledExceptionPtr);
		}

		if (waiterToInvoke) { waiterToInvoke(); }
	}

	/** Caller must hold s.lock. */
	bool verifySlotIsManagedUnlocked(const Slot &slot) const
	{
		for (const auto &trackedSlot : s.rsrc.slots)
		{
			if (&trackedSlot == &slot) {
				return true;
			}
		}

		return false;
	}

	/** Caller must hold s.lock. */
	void pruneRetiredSlotsUnlocked()
	{
		for (auto it = s.rsrc.slots.begin(); it != s.rsrc.slots.end();)
		{
			if (it->leaseStatus == SlotLeaseStatus::RETIRED) {
				it = s.rsrc.slots.erase(it);
			} else {
				++it;
			}
		}
	}

	/** Caller must hold s.lock. */
	std::function<void()> takeDrainWaiterIfDrainedUnlocked()
	{
		if (s.rsrc.admissionOpen) {
			return {};
		}

		if (countUnsettledSlotsUnlocked() != 0) {
			return {};
		}

		return std::exchange(s.rsrc.drainWaiter, {});
	}

public:
	mutable sscl::SharedResourceGroup<sscl::SpinLock, State> s;
};

} // namespace sscl::co

#endif // NON_VIRAL_TASK_NURSERY_H
