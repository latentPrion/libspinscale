#ifndef COMPONENT_H
#define COMPONENT_H

#include <config.h>
#include <atomic>
#include <memory>
#include <spinscale/cps/callback.h>
#include <spinscale/puppetApplication.h>

namespace sscl {

class ComponentThread;
class PuppetThread;

/**	EXPLANATION:
 * Components are API-exposing sub-components of an application. They are used
 * aggregate the resources and API of some logically distinct sub-system into
 * a single abstract entity. Basically, a component is a way to bind some APIs
 * and resources to a particular thread. Ideally, all accesses to the resources
 * of a component should be made through the component's APIs.
 *
 * Multiple components can share the same thread; and for this reason, each
 * component must be supplied with a reference to the thread it shares.
 * This amounts to saying that a single thread may expose and serve multiple
 * APIs.
 */
class Component
{
public:
	Component(const std::shared_ptr<ComponentThread> &thread);
	~Component() = default;

public:
	std::shared_ptr<ComponentThread> thread;

public:
};

class PuppetComponent
:	public Component
{
public:
	virtual void handleLoopExceptionHook() = 0;

	PuppetComponent(
		PuppetApplication &parent,
		const std::shared_ptr<PuppetThread> &thread);
	~PuppetComponent() = default;

	static void defaultPuppetMain(const PuppetThread::EntryFnArguments &args);

public:
	PuppetApplication &parent;

protected:
	virtual void postJoltHook() {}
	virtual void preLoopHook() {}
	virtual void postLoopHook() {}
};

namespace pptr {

class PuppeteerComponent
:	public Component
{
public:
	virtual void handleLoopExceptionHook() = 0;

	PuppeteerComponent(const std::shared_ptr<PuppeteerThread> &thread);
	~PuppeteerComponent() = default;

	static void defaultPuppeteerMain(
		const PuppeteerThread::EntryFnArguments &args);

protected:
	virtual void postJoltHook() {}
	virtual void tryBlock1Hook() {}
	virtual void preLoopHook() {}
	virtual void postLoopHook() {}
	virtual void postTryBlock1CatchHook() {}
	virtual void handleTryBlock1TypedException(const std::exception& e);
	virtual void handleTryBlock1UnknownException();
};

extern std::atomic<int> exitCode;

} // namespace pptr

} // namespace sscl

#endif // COMPONENT_H
