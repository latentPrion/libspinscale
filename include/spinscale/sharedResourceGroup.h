#ifndef SHARED_RESOURCE_GROUP_H
#define SHARED_RESOURCE_GROUP_H

#include <string>
#include <utility>

namespace sscl {

template <typename LockType, typename ResourceType>
class SharedResourceGroup
{
public:
	SharedResourceGroup() = default;

	explicit SharedResourceGroup(const std::string& lockName)
	: lock(lockName)
	{}

	SharedResourceGroup(
		const std::string& lockName, const ResourceType& initialRsrc)
	: lock(lockName), rsrc(initialRsrc)
	{}

	SharedResourceGroup(
		const std::string& lockName, ResourceType&& initialRsrc)
	: lock(lockName), rsrc(std::move(initialRsrc))
	{}

	~SharedResourceGroup() = default;

	LockType lock;
	ResourceType rsrc;
};

} // namespace sscl

#endif // SHARED_RESOURCE_GROUP_H
