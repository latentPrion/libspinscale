#include <iostream>
#include <pthread.h>
#include <spinscale/component.h>
#include <spinscale/componentThread.h>
#include <spinscale/puppetApplication.h>

namespace sscl {

Component::Component(const std::shared_ptr<ComponentThread> &thread)
: thread(thread)
{
}

PuppetComponent::PuppetComponent(
	PuppetApplication &parent, const std::shared_ptr<PuppetThread> &thread)
:	Component(thread),
parent(parent)
{
}

void PuppetComponent::defaultPuppetMain(
	const PuppetThread::EntryFnArguments &args)
{
	PuppetThread &thr = args.usableBeforeJolt;
	PuppetComponent &comp = args.useOnlyAfterJolt;

	if (args.preJoltHook) { args.preJoltHook(thr); }

	/**	FIXME:
	 * Figure out why we don't call restart() here, and then explicitly document
	 * it.
	 */
	thr.getIoContext().run();
	thr.initializeTls();

	comp.postJoltHook();
	comp.preLoopHook();

	/* We loop here because when an exception is caught, we need to first catch
	 * it in the catch blocks and invoke handleLoopExceptionHook so the
	 * application can respond (e.g. notify a controller). We then re-enter
	 * the loop to await control messages.
	 *
	 * We can't just exit on our own. Rather, keepLooping must be set to false
	 * by the application when shutdown is desired.
	 */
	for (thr.keepLooping = true; thr.keepLooping;)
	{
		bool sendExceptionInd = false;

		try {
			/**		EXPLANATION:
			 * This reset() call is crucial for async bridging patterns
			 * to work.
			 * When the outermost thread's io_context is stop()ped (e.g.,
			 * from JOLT sequence), it won't process any new work until
			 * restart() is called, even if nested async operations try to
			 * post work to it. This means async bridges invoked from
			 * the outermost thread main sequence won't work until this
			 * restart() call.
			 */
			thr.getIoContext().restart();
			thr.getIoContext().run();
		}
		catch (const std::exception& e)
		{
			sendExceptionInd = true;
			std::cerr << thr.name << ":" << __func__
				<< ": Exception occurred: " << e.what() << "\n";
		}
		catch (...)
		{
			sendExceptionInd = true;
			std::cerr << thr.name << ":" << __func__
				<< ": Unknown exception occurred" << "\n";
		}

		if (sendExceptionInd)
		{
			comp.handleLoopExceptionHook();
		}
	}

	comp.postLoopHook();

	/** exitThreadReq signals after both dual-posts are queued; wait before
	 * destroying contexts so the pause post cannot race teardown.
	 */
	thr.syncAwaitExitDualPostsCompleted();

	/** Abandoned dual-posted exit ops (and any other deferred handlers) are
	 * destroyed here via ~io_context, after run() has returned.
	 */
	thr.destroyIoContextsAfterMainLoop();
}

namespace pptr {

PuppeteerComponent::PuppeteerComponent(
	const std::shared_ptr<sscl::PuppeteerThread> &thread)
:	sscl::Component(thread)
{
}

} // namespace pptr

} // namespace sscl
