#ifndef SPINSCALE_TEST_SUPPORT_BAKED_DEVICE_CATALOG_H
#define SPINSCALE_TEST_SUPPORT_BAKED_DEVICE_CATALOG_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <bakedCameraProfiles.h>

namespace sscl::tests {

inline std::vector<const test_fixtures::BakedCameraProfile *>
profilesForMachine(const char *machineTag)
{
	std::vector<const test_fixtures::BakedCameraProfile *> matches;

	for (std::size_t i = 0; i < test_fixtures::bakedCameraProfileCount; ++i)
	{
		const test_fixtures::BakedCameraProfile& profile =
			test_fixtures::bakedCameraProfiles[i];

		if (std::string(profile.machineTag) == machineTag) {
			matches.push_back(&profile);
		}
	}

	return matches;
}

inline std::optional<const test_fixtures::BakedCameraProfile *>
findProfileByTag(const char *machineTag, const char *profileTag)
{
	for (std::size_t i = 0; i < test_fixtures::bakedCameraProfileCount; ++i)
	{
		const test_fixtures::BakedCameraProfile& profile =
			test_fixtures::bakedCameraProfiles[i];

		if (std::string(profile.machineTag) == machineTag
			&& std::string(profile.profileTag) == profileTag)
		{
			return &profile;
		}
	}

	return std::nullopt;
}

inline std::vector<const test_fixtures::BakedCameraProfile *>
requiredProfilesForMachine(const char *machineTag)
{
	std::vector<const test_fixtures::BakedCameraProfile *> matches;

	for (std::size_t i = 0; i < test_fixtures::bakedCameraProfileCount; ++i)
	{
		const test_fixtures::BakedCameraProfile& profile =
			test_fixtures::bakedCameraProfiles[i];

		if (std::string(profile.machineTag) == machineTag
			&& profile.requiredOnMachine)
		{
			matches.push_back(&profile);
		}
	}

	return matches;
}

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_BAKED_DEVICE_CATALOG_H
