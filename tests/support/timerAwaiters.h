#ifndef SPINSCALE_TEST_SUPPORT_TIMER_AWAITERS_H
#define SPINSCALE_TEST_SUPPORT_TIMER_AWAITERS_H

#include <boostAsioLinkageFix.h>

#include <chrono>
#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>

namespace sscl::tests {

using SharedSteadyTimer = std::shared_ptr<boost::asio::steady_timer>;

/* Keep historical names as aliases so existing spinscale tests stay readable. */
using SharedDeadlineTimer = SharedSteadyTimer;

class CancelableDeadlineTimerRegistry
{
public:
	void clear()
	{
		std::lock_guard<std::mutex> guard(mutex);
		timersByLabel.clear();
	}

	void registerTimer(
		int labelMilliseconds,
		const SharedSteadyTimer &timer)
	{
		std::lock_guard<std::mutex> guard(mutex);
		timersByLabel[labelMilliseconds] = timer;
	}

	void cancel(int labelMilliseconds)
	{
		std::lock_guard<std::mutex> guard(mutex);
		const auto iterator = timersByLabel.find(labelMilliseconds);

		if (iterator == timersByLabel.end()) {
			throw std::runtime_error(
				"No cancelable steady_timer registered for label "
				+ std::to_string(labelMilliseconds));
		}

		const SharedSteadyTimer timer = iterator->second.lock();

		if (!timer) {
			throw std::runtime_error(
				"Cancelable steady_timer expired before cancel for label "
				+ std::to_string(labelMilliseconds));
		}

		timer->cancel();
	}

private:
	std::mutex mutex;
	std::unordered_map<int, std::weak_ptr<boost::asio::steady_timer>>
		timersByLabel;
};

struct DeadlineTimerAwaiter
{
	DeadlineTimerAwaiter(
		boost::asio::io_context &ioContext,
		int delayMilliseconds)
	: timer(std::make_shared<boost::asio::steady_timer>(ioContext))
	{
		start(delayMilliseconds);
	}

	DeadlineTimerAwaiter(
		SharedSteadyTimer sharedTimer,
		int delayMilliseconds)
	: timer(std::move(sharedTimer))
	{
		start(delayMilliseconds);
	}

	bool await_ready() const noexcept
		{ return waitCompleted; }

	bool await_suspend(std::coroutine_handle<> handle) noexcept
	{
		resumeHandle = handle;
		return !waitCompleted;
	}

	boost::system::error_code await_resume() const noexcept
		{ return completionErrorCode; }

private:
	void start(int delayMilliseconds)
	{
		timer->expires_after(
			std::chrono::milliseconds(delayMilliseconds));
		timer->async_wait(
			[this](const boost::system::error_code &errorCode)
			{
				completionErrorCode = errorCode;
				waitCompleted = true;
				if (resumeHandle) {
					resumeHandle.resume();
				}
			});
	}

	SharedSteadyTimer timer;
	boost::system::error_code completionErrorCode;
	bool waitCompleted = false;
	std::coroutine_handle<> resumeHandle;
};

struct RegisteredDeadlineTimerAwaiter
{
	RegisteredDeadlineTimerAwaiter(
		boost::asio::io_context &ioContext,
		int delayMilliseconds,
		int registrationLabelMilliseconds,
		CancelableDeadlineTimerRegistry &registry)
	: timer(std::make_shared<boost::asio::steady_timer>(ioContext))
	{
		registry.registerTimer(registrationLabelMilliseconds, timer);
		waiter.emplace(timer, delayMilliseconds);
	}

	bool await_ready() const noexcept
		{ return waiter->await_ready(); }

	bool await_suspend(std::coroutine_handle<> handle) noexcept
		{ return waiter->await_suspend(handle); }

	boost::system::error_code await_resume() const noexcept
		{ return waiter->await_resume(); }

	SharedSteadyTimer timer;
	std::optional<DeadlineTimerAwaiter> waiter;
};

inline void throwIfTimerWaitFailed(
	const boost::system::error_code &waitError)
{
	if (waitError) {
		throw std::runtime_error(
			"steady_timer wait failed: " + waitError.message());
	}
}

inline bool timerWasCanceled(const boost::system::error_code &waitError)
{
	return waitError == boost::asio::error::operation_aborted;
}

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_TIMER_AWAITERS_H
