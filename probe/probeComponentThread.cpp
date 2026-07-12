#include <probe/probeComponentThread.h>

#include <iostream>

#include <spinscale/component.h>

namespace sscl::probe {

namespace {

constexpr sscl::ThreadId PROBE_PUPPETEER_THREAD_ID = 2;

class ProbeDummyPuppeteerComponent
:	public sscl::pptr::PuppeteerComponent
{
public:
	explicit ProbeDummyPuppeteerComponent(
		const std::shared_ptr<sscl::PuppeteerThread>& componentThreadIn)
	:	sscl::pptr::PuppeteerComponent(componentThreadIn)
	{}

	void handleLoopExceptionHook() override
	{
		std::cerr << "ProbeComponentThreadHarness: puppeteer loop exception\n";
	}
};

void probePuppeteerMain(
	const sscl::PuppeteerThread::EntryFnArguments& args,
	const std::function<void(
		const std::shared_ptr<sscl::ComponentThread>&)>& work,
	std::promise<std::exception_ptr>& donePromise)
{
	sscl::PuppeteerThread& thr = args.usableBeforeJolt;
	thr.initializeTls();
	sscl::ComponentThread::setPuppeteerThreadId(PROBE_PUPPETEER_THREAD_ID);

	std::shared_ptr<sscl::PuppeteerThread> thrPtr =
		std::static_pointer_cast<sscl::PuppeteerThread>(thr.shared_from_this());
	sscl::ComponentThread::setPuppeteerThread(thrPtr);

	try {
		work(thrPtr);
		donePromise.set_value(nullptr);
	}
	catch (...) {
		donePromise.set_value(std::current_exception());
	}

	thr.getIoContext().stop();
}

} // namespace

ProbeComponentThreadHarness::ProbeComponentThreadHarness(
	const char *threadName)
:	threadName(threadName),
	dummyComponent(std::make_shared<ProbeDummyPuppeteerComponent>(
		std::shared_ptr<sscl::PuppeteerThread>()))
{}

ProbeComponentThreadHarness::~ProbeComponentThreadHarness() = default;

std::shared_ptr<sscl::ComponentThread>
ProbeComponentThreadHarness::componentThread() const
{
	return lastComponentThread;
}

void ProbeComponentThreadHarness::runSync(
	const std::function<void(
		const std::shared_ptr<sscl::ComponentThread>&)>& work)
{
	std::promise<std::exception_ptr> donePromise;
	std::future<std::exception_ptr> doneFuture = donePromise.get_future();

	std::shared_ptr<sscl::PuppeteerThread> runThread =
		std::make_shared<sscl::PuppeteerThread>(
			PROBE_PUPPETEER_THREAD_ID,
			threadName,
			[&work, &donePromise](
				const sscl::PuppeteerThread::EntryFnArguments& args)
			{
				probePuppeteerMain(args, work, donePromise);
			},
			*dummyComponent,
			nullptr);

	dummyComponent->thread = runThread;
	lastComponentThread = runThread;
	runThread->thread.join();

	std::exception_ptr probeException = doneFuture.get();
	if (probeException) {
		std::rethrow_exception(probeException);
	}
}

void runNonViralNurseryOnComponentThread(
	const std::shared_ptr<sscl::ComponentThread>& componentThread,
	std::function<sscl::co::NonViralNonPostingInvoker(
		sscl::co::NonViralTaskNursery::Slot::Lease&)> invokerFactory,
	std::chrono::milliseconds timeout)
{
	(void)timeout;

	sscl::co::NonViralTaskNursery nursery;
	nursery.openAdmission();
	nursery.launch(
		[&invokerFactory](sscl::co::NonViralTaskNursery::Slot::Lease& lease)
		{
			return invokerFactory(lease);
		});
	nursery.closeAdmission();
	nursery.syncAwaitAllSettlements(componentThread->getIoContext());
}

} // namespace sscl::probe
