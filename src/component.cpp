#include <spinscale/component.h>
#include <spinscale/puppetApplication.h>
#include <spinscale/marionette.h>

namespace sscl {

Component::Component(const std::shared_ptr<ComponentThread> &thread)
:	thread(thread)
{
}

PuppetComponent::PuppetComponent(
	PuppetApplication &parent, const std::shared_ptr<ComponentThread> &thread)
:	Component(thread),
parent(parent)
{
}

namespace mrntt {

MarionetteComponent::MarionetteComponent(
	const std::shared_ptr<sscl::ComponentThread> &thread)
:	sscl::Component(thread)
{
}

} // namespace mrntt
} // namespace sscl
