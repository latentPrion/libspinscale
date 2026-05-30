#include <iostream>
#include <pthread.h>
#include <spinscale/component.h>
#include <spinscale/componentThread.h>

namespace sscl {
namespace pptr {

std::atomic<int> exitCode{0};

void PuppeteerComponent::defaultPuppeteerMain(
	const PuppeteerThread::EntryFnArguments &args)
{
	PuppeteerThread &thr = args.usableBeforeJolt;
	PuppeteerComponent &comp = args.useOnlyAfterJolt;

	if (args.preJoltHook) { args.preJoltHook(thr); }

	thr.getIoContext().restart();
	thr.getIoContext().run();
	thr.initializeTls();

	comp.postJoltHook();

	try {
		comp.tryBlock1Hook();
		comp.preLoopHook();

		/* We loop here because when an exception occurs, we need to
		 * both direct the puppet threads to exit gracefully, and then we
		 * also need to post messages to our own event loop to initiate
		 * our own orderly exit. So we loop here to re-enter the event
		 * loop, both to receive the ACK messages from the puppet
		 * threads, and to post messages to our own event loop to
		 * initiate our own orderly exit.
		 */
		for (thr.keepLooping = true; thr.keepLooping;)
		{
			bool sendExceptionInd = false;

			try {
				/**		EXPLANATION:
				 * This restart() call is crucial for async bridging
				 * patterns to work.
				 * When the outermost thread's io_context is stop()ped
				 * (e.g., from JOLT sequence), it won't process any new
				 * work until restart() is called, even if nested async
				 * operations try to post work to it. This means async
				 * bridges invoked from the outermost thread main sequence
				 * won't work until this restart() call.
				 */
				thr.getIoContext().restart();
				thr.getIoContext().run();
			}
			catch (const std::exception& e)
			{
				sendExceptionInd = true;
				std::cerr << thr.name << ":main: Exception occurred: "
					<< e.what() << "\n";
			}
			catch (...)
			{
				sendExceptionInd = true;
				std::cerr << thr.name
					<< ":main: Unknown exception occurred" << "\n";
			}

			if (sendExceptionInd)
			{
				comp.handleLoopExceptionHook();
			}
		}

		comp.postLoopHook();
	}
	catch (const std::exception& e)
	{
		comp.handleTryBlock1TypedException(e);
	}
	catch (...)
	{
		comp.handleTryBlock1UnknownException();
	}

	comp.postTryBlock1CatchHook();
}

void PuppeteerComponent::handleTryBlock1TypedException(const std::exception& e)
{
	std::cerr << "main: Exception occurred: " << e.what() << std::endl;
}

void PuppeteerComponent::handleTryBlock1UnknownException()
{
	std::cerr << "main: Unknown exception occurred" << std::endl;
}

} // namespace pptr
} // namespace sscl
