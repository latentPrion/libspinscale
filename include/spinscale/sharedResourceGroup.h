#ifndef SHARED_RESOURCE_GROUP_H
#define SHARED_RESOURCE_GROUP_H

#include <string>

namespace sscl {

template <typename LockType, typename ResourceType>
class SharedResourceGroup
{
public:
	SharedResourceGroup() = default;

	explicit SharedResourceGroup(const std::string& lockName)
	: lock(lockName)
	{}

	~SharedResourceGroup() = default;

	LockType lock;
	ResourceType rsrc;
};

} // namespace sscl

#endif // SHARED_RESOURCE_GROUP_H
