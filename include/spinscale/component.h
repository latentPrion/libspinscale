#ifndef COMPONENT_H
#define COMPONENT_H

#include <config.h>
#include <memory>
#include <functional>
#include <spinscale/callback.h>
#include <spinscale/puppetApplication.h>

namespace sscl {

class ComponentThread;

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
		const std::shared_ptr<ComponentThread> &thread);
	~PuppetComponent() = default;

public:
	PuppetApplication &parent;
};

} // namespace sscl

#endif // COMPONENT_H
