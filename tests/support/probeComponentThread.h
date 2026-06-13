#ifndef SPINSCALE_TEST_SUPPORT_PROBE_COMPONENT_THREAD_H
#define SPINSCALE_TEST_SUPPORT_PROBE_COMPONENT_THREAD_H

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>

#include <spinscale/componentThread.h>
#include <spinscale/co/invokers.h>
#include <spinscale/co/nonViralTaskNursery.h>

namespace sscl::tests {

constexpr std::chrono::milliseconds defaultProbeTaskTimeout{10000};

void runNonViralNurseryOnComponentThread(
	const std::shared_ptr<sscl::ComponentThread>& componentThread,
	std::function<sscl::co::NonViralNonPostingInvoker(
		sscl::co::NonViralTaskNursery::Slot::Lease&)> invokerFactory,
	std::chrono::milliseconds timeout = defaultProbeTaskTimeout);

class ProbeComponentThreadHarness
{
public:
	explicit ProbeComponentThreadHarness(
		const char *threadName = "spinscale-probe");
	~ProbeComponentThreadHarness();

	ProbeComponentThreadHarness(const ProbeComponentThreadHarness &) = delete;
	ProbeComponentThreadHarness &operator=(
		const ProbeComponentThreadHarness &) = delete;

	std::shared_ptr<sscl::ComponentThread> componentThread() const;

	void runSync(
		const std::function<void(
			const std::shared_ptr<sscl::ComponentThread>&)>& work);

	template <typename InvokerFactory>
	void runNonViralNurseryTask(
		InvokerFactory &&invokerFactory,
		std::chrono::milliseconds timeout = defaultProbeTaskTimeout)
	{
		runSync(
			[this, &invokerFactory, timeout](
				const std::shared_ptr<sscl::ComponentThread>& componentThread)
			{
				sscl::tests::runNonViralNurseryOnComponentThread(
					componentThread,
					std::forward<InvokerFactory>(invokerFactory),
					timeout);
			});
	}

private:
	std::string threadName;
	std::shared_ptr<sscl::pptr::PuppeteerComponent> dummyComponent;
	std::shared_ptr<sscl::ComponentThread> lastComponentThread;
};

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_PROBE_COMPONENT_THREAD_H
