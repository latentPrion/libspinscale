#ifndef PUPPET_APPLICATION_H
#define PUPPET_APPLICATION_H

#include <config.h>
#include <functional>
#include <memory>
#include <vector>
#include <spinscale/callback.h>
#include <spinscale/componentThread.h>

namespace sscl {

class PuppetApplication
:	public std::enable_shared_from_this<PuppetApplication>
{
public:
	PuppetApplication(
		const std::vector<std::shared_ptr<PuppetThread>> &threads);
	~PuppetApplication() = default;

	// Thread management methods
	typedef std::function<void()> puppetThreadLifetimeMgmtOpCbFn;
	void joltAllPuppetThreadsReq(
		Callback<puppetThreadLifetimeMgmtOpCbFn> callback);
	void startAllPuppetThreadsReq(
		Callback<puppetThreadLifetimeMgmtOpCbFn> callback);
	void pauseAllPuppetThreadsReq(
		Callback<puppetThreadLifetimeMgmtOpCbFn> callback);
	void resumeAllPuppetThreadsReq(
		Callback<puppetThreadLifetimeMgmtOpCbFn> callback);
	void exitAllPuppetThreadsReq(
		Callback<puppetThreadLifetimeMgmtOpCbFn> callback);

	// CPU distribution method
	void distributeAndPinThreadsAcrossCpus();

protected:
	// Collection of PuppetThread instances
	std::vector<std::shared_ptr<PuppetThread>> componentThreads;

	/**
	 * Indicates whether all puppet threads have been JOLTed at least once.
	 *
	 * JOLTing serves two critical purposes:
	 *
	 * 1. **Global Constructor Sequencing**: Since pthreads begin executing while
	 *    global constructors are still being executed, globally defined pthreads
	 *    cannot depend on global objects having been constructed. JOLTing is done
	 *    by the CRT's main thread within main(), which provides a sequencing
	 *    guarantee that global constructors have been called.
	 *
	 * 2. **shared_from_this Safety**: shared_from_this() requires a prior
	 *    shared_ptr handle to be established. The global list of
	 *    shared_ptr<ComponentThread> guarantees that at least one shared_ptr to
	 *    each ComponentThread has been initialized before JOLTing occurs.
	 *
	 * This flag ensures that JOLTing happens exactly once and provides
	 * a synchronization point for the entire system initialization.
	 */
	bool threadsHaveBeenJolted = false;

private:
	class PuppetThreadLifetimeMgmtOp;
};

} // namespace sscl

#endif // PUPPET_APPLICATION_H
