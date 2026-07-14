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

	/**	EXPLANATION:
	 * Precedence: process getenv wins over compiled file-store values unless
	 * bypassProcessEnvironment is true (file store only).
	 */
	std::optional<std::string> find(
		std::string_view name,
		bool bypassProcessEnvironment = false) const;

	/** Throws if find() returns nullopt. */
	std::string get(
		std::string_view name,
		bool bypassProcessEnvironment = false) const;

	/**	EXPLANATION:
	 * Typed int accessors. defaultValue applies only when find() is nullopt;
	 * nullopt defaultValue with a missing key throws. A present value that fails
	 * to parse or fails the positivity constraint always throws.
	 */
	int getInt(
		std::string_view name,
		std::optional<int> defaultValue = std::nullopt) const
	{
		return getIntWithConstraint(name, defaultValue, IntConstraint::Any);
	}

	/** Parsed value must be >= 0. */
	int getPositiveInt(
		std::string_view name,
		std::optional<int> defaultValue = std::nullopt) const
	{
		return getIntWithConstraint(
			name, defaultValue, IntConstraint::NonNegative);
	}

	/** Parsed value must be > 0. */
	int getPositiveNonZeroInt(
		std::string_view name,
		std::optional<int> defaultValue = std::nullopt) const
	{
		return getIntWithConstraint(
			name, defaultValue, IntConstraint::PositiveNonZero);
	}

private:
	/** dotenv line parsing owned by EnvKvStore (definition in .cpp). */
	class DotenvParser;

	enum class IntConstraint
	{
		Any,
		NonNegative,
		PositiveNonZero,
	};

	static int parseInt(std::string_view name, const std::string &raw);
	int getIntWithConstraint(
		std::string_view name,
		std::optional<int> defaultValue,
		IntConstraint constraint) const;

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
