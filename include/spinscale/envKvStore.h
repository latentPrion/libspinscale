#ifndef SPINSCALE_ENV_KV_STORE_H
#define SPINSCALE_ENV_KV_STORE_H

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sscl {

class EnvKvStore
{
public:
	explicit EnvKvStore(
		const std::vector<std::filesystem::path> &envFilePaths,
		std::ostream &warningStream);
	explicit EnvKvStore(
		const std::vector<std::filesystem::path> &envFilePaths);

	std::optional<std::string> get(std::string_view name) const;

private:
	void loadFiles(
		const std::vector<std::filesystem::path> &envFilePaths,
		std::ostream &warningStream);
	void loadFile(
		const std::filesystem::path &envFilePath,
		std::ostream &warningStream);
	void storeValue(
		const std::filesystem::path &envFilePath,
		const std::string &name,
		const std::string &value,
		std::ostream &warningStream);

private:
	std::unordered_map<std::string, std::string> values;
};

} // namespace sscl

#endif // SPINSCALE_ENV_KV_STORE_H
