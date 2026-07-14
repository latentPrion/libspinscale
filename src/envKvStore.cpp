#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <spinscale/envKvStore.h>

namespace sscl {

class EnvKvStore::DotenvParser
{
public:
	static bool lineIsBlankOrComment(const std::string &line)
	{
		std::string trimmed = trim(line);
		return trimmed.empty() || trimmed.front() == '#';
	}

	static std::pair<std::string, std::string> parseAssignment(
		const std::filesystem::path &envFilePath,
		std::size_t lineNumber,
		const std::string &line)
	{
		std::size_t separator = line.find('=');
		if (separator == std::string::npos)
		{
			throw makeParseError(
				envFilePath, lineNumber, "Expected KEY=value.");
		}

		std::string name = trim(std::string_view(line).substr(0, separator));
		if (!nameIsValid(name))
		{
			throw makeParseError(
				envFilePath, lineNumber, "Invalid variable name.");
		}

		return {
			std::move(name),
			parseValue(
				envFilePath,
				lineNumber,
				std::string_view(line).substr(separator + 1))};
	}

private:
	static std::string trim(std::string_view value)
	{
		auto begin = std::ranges::find_if_not(
			value, [](unsigned char c) { return std::isspace(c); });
		auto rbegin = std::ranges::find_if_not(
			value | std::views::reverse,
			[](unsigned char c) { return std::isspace(c); });
		auto end = rbegin.base();
		if (begin >= end) { return {}; }
		return std::string(begin, end);
	}

	static bool characterIsValidNameStart(char c)
	{
		return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
	}

	static bool characterIsValidNameBody(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	static bool nameIsValid(std::string_view name)
	{
		if (name.empty() || !characterIsValidNameStart(name.front())) { return false; }
		return std::ranges::all_of(name.substr(1), characterIsValidNameBody);
	}

	static std::runtime_error makeParseError(
		const std::filesystem::path &envFilePath,
		std::size_t lineNumber,
		const std::string &message)
	{
		std::ostringstream stream;
		stream << envFilePath << ":" << lineNumber << ": " << message;
		return std::runtime_error(stream.str());
	}

	static std::size_t findClosingQuote(
		const std::filesystem::path &envFilePath,
		std::size_t lineNumber,
		std::string_view value)
	{
		char quote = value.front();
		bool escapeNext = false;
		for (std::size_t i = 1; i < value.size(); ++i)
		{
			if (escapeNext)
			{
				escapeNext = false;
				continue;
			}
			if (quote == '"' && value[i] == '\\')
			{
				escapeNext = true;
				continue;
			}
			if (value[i] == quote) { return i; }
		}
		throw makeParseError(
			envFilePath, lineNumber, "Unterminated quoted value.");
	}

	static std::string decodeDoubleQuotedValue(std::string_view value)
	{
		std::string decoded;
		decoded.reserve(value.size());
		bool escapeNext = false;
		for (char c : value)
		{
			if (!escapeNext && c == '\\')
			{
				escapeNext = true;
				continue;
			}
			if (escapeNext)
			{
				switch (c)
				{
					case 'n':
						decoded.push_back('\n');
						break;
					case 'r':
						decoded.push_back('\r');
						break;
					case 't':
						decoded.push_back('\t');
						break;
					default:
						decoded.push_back(c);
						break;
				}
				escapeNext = false;
				continue;
			}
			decoded.push_back(c);
		}
		if (escapeNext) { decoded.push_back('\\'); }
		return decoded;
	}

	static std::string parseQuotedValue(
		const std::filesystem::path &envFilePath,
		std::size_t lineNumber,
		std::string_view value)
	{
		char quote = value.front();
		std::size_t closingQuote =
			findClosingQuote(envFilePath, lineNumber, value);
		std::string trailing = trim(value.substr(closingQuote + 1));
		if (!trailing.empty() && trailing.front() != '#')
		{
			throw makeParseError(
				envFilePath,
				lineNumber,
				"Unexpected text after quoted value.");
		}
		std::string_view quotedBody = value.substr(1, closingQuote - 1);
		if (quote == '"') { return decodeDoubleQuotedValue(quotedBody); }
		return std::string(quotedBody);
	}

	static std::string parseValue(
		const std::filesystem::path &envFilePath,
		std::size_t lineNumber,
		std::string_view rawValue)
	{
		std::string value = trim(rawValue);
		if (value.empty()) { return {}; }
		if (value.front() == '\'' || value.front() == '"') { return parseQuotedValue(envFilePath, lineNumber, value); }
		return trim(value.substr(0, value.find('#')));
	}
};

EnvKvStore::EnvKvStore(
	const std::vector<std::filesystem::path> &envFilePaths,
	std::ostream &warningStream)
{
	loadFiles(envFilePaths, warningStream);
}

EnvKvStore::EnvKvStore(
	const std::vector<std::filesystem::path> &envFilePaths)
:	EnvKvStore(envFilePaths, std::cerr)
{}

void EnvKvStore::loadFiles(
	const std::vector<std::filesystem::path> &envFilePaths,
	std::ostream &warningStream)
{
	for (const std::filesystem::path &envFilePath : envFilePaths)
	{
		loadFile(envFilePath, warningStream);
	}
}

std::optional<std::string> EnvKvStore::find(
	std::string_view name,
	bool bypassProcessEnvironment) const
{
	if (!bypassProcessEnvironment)
	{
		std::string ownedName(name);
		if (const char *value = std::getenv(ownedName.c_str())) {
			return std::string(value);
		}
	}

	auto value = values.find(std::string(name));
	if (value == values.end()) { return std::nullopt; }
	return value->second;
}

std::string EnvKvStore::get(
	std::string_view name,
	bool bypassProcessEnvironment) const
{
	std::optional<std::string> value = find(name, bypassProcessEnvironment);
	if (!value.has_value())
	{
		throw std::runtime_error(
			std::string("EnvKvStore: missing key '")
			+ std::string(name)
			+ "'");
	}
	return *value;
}

int EnvKvStore::parseInt(std::string_view name, const std::string &raw)
{
	try
	{
		std::size_t consumed = 0;
		const long parsed = std::stol(raw, &consumed);
		if (consumed != raw.size())
		{
			throw std::runtime_error(
				std::string("EnvKvStore: '")
				+ std::string(name)
				+ "' must be an integer, got: "
				+ raw);
		}
		if (parsed < std::numeric_limits<int>::min()
			|| parsed > std::numeric_limits<int>::max())
		{
			throw std::runtime_error(
				std::string("EnvKvStore: '")
				+ std::string(name)
				+ "' is out of int range, got: "
				+ raw);
		}
		return static_cast<int>(parsed);
	}
	catch (const std::runtime_error &)
	{
		throw;
	}
	catch (const std::exception &)
	{
		throw std::runtime_error(
			std::string("EnvKvStore: failed to parse '")
			+ std::string(name)
			+ "' as an integer, got: "
			+ raw);
	}
}

int EnvKvStore::getIntWithConstraint(
	std::string_view name,
	std::optional<int> defaultValue,
	IntConstraint constraint) const
{
	const std::optional<std::string> raw = find(name);
	if (!raw.has_value())
	{
		if (!defaultValue.has_value())
		{
			throw std::runtime_error(
				std::string("EnvKvStore: missing key '")
				+ std::string(name)
				+ "'");
		}
		return *defaultValue;
	}

	const int parsed = parseInt(name, *raw);
	if (constraint == IntConstraint::NonNegative && parsed < 0)
	{
		throw std::runtime_error(
			std::string("EnvKvStore: '")
			+ std::string(name)
			+ "' must be a non-negative integer, got: "
			+ *raw);
	}
	if (constraint == IntConstraint::PositiveNonZero && parsed <= 0)
	{
		throw std::runtime_error(
			std::string("EnvKvStore: '")
			+ std::string(name)
			+ "' must be a positive non-zero integer, got: "
			+ *raw);
	}
	return parsed;
}

void EnvKvStore::loadFile(
	const std::filesystem::path &envFilePath,
	std::ostream &warningStream)
{
	std::ifstream file(envFilePath);
	if (!file)
	{
		throw std::runtime_error(
			"Failed to open env file: " + envFilePath.string());
	}

	std::string line;
	std::size_t lineNumber = 0;
	while (std::getline(file, line))
	{
		++lineNumber;
		if (DotenvParser::lineIsBlankOrComment(line)) { continue; }
		auto [name, value] =
			DotenvParser::parseAssignment(envFilePath, lineNumber, line);
		storeValue(envFilePath, name, value, warningStream);
	}
}

void EnvKvStore::storeValue(
	const std::filesystem::path &envFilePath,
	const std::string &name,
	const std::string &value,
	std::ostream &warningStream)
{
	if (auto oldValue = values.find(name); oldValue != values.end())
	{
		warningStream << "Warning: env file " << envFilePath
			<< " overwrites " << name << " from `" << oldValue->second
			<< "` to `" << value << "`.\n";
		oldValue->second = value;
		return;
	}
	values.emplace(name, value);
}

} // namespace sscl
