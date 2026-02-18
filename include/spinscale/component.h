#ifndef COMPONENT_H
#define COMPONENT_H

#include <config.h>
#include <memory>
#include <functional>
#include <spinscale/callback.h>
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
	PuppetComponent(
		PuppetApplication &parent,
		const std::shared_ptr<PuppetThread> &thread);
	~PuppetComponent() = default;

public:
	PuppetApplication &parent;
};

} // namespace sscl

#endif // COMPONENT_H
