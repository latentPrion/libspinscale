#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ranges>
#include <sstream>
#include <stdexcept>

#include <spinscale/envKvStore.h>

namespace sscl {
namespace {

std::string trim(std::string_view value)
{
	auto begin = std::ranges::find_if_not(
		value, [](unsigned char c) { return std::isspace(c); });
	auto rbegin = std::ranges::find_if_not(
		value | std::views::reverse,
		[](unsigned char c) { return std::isspace(c); });
	auto end = rbegin.base();
	if (begin >= end)
	{
		return {};
	}
	return std::string(begin, end);
}

bool characterIsValidNameStart(char c)
{
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool characterIsValidNameBody(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool nameIsValid(std::string_view name)
{
	if (name.empty() || !characterIsValidNameStart(name.front()))
	{
		return false;
	}
	return std::ranges::all_of(name.substr(1), characterIsValidNameBody);
}

std::runtime_error makeParseError(
	const std::filesystem::path &envFilePath,
	std::size_t lineNumber,
	const std::string &message)
{
	std::ostringstream stream;
	stream << envFilePath << ":" << lineNumber << ": " << message;
	return std::runtime_error(stream.str());
}

bool lineIsBlankOrComment(const std::string &line)
{
	std::string trimmed = trim(line);
	return trimmed.empty() || trimmed.front() == '#';
}

std::size_t findClosingQuote(
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
		if (value[i] == quote)
		{
			return i;
		}
	}
	throw makeParseError(envFilePath, lineNumber, "Unterminated quoted value.");
}

std::string decodeDoubleQuotedValue(std::string_view value)
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
	if (escapeNext)
	{
		decoded.push_back('\\');
	}
	return decoded;
}

std::string parseQuotedValue(
	const std::filesystem::path &envFilePath,
	std::size_t lineNumber,
	std::string_view value)
{
	char quote = value.front();
	std::size_t closingQuote = findClosingQuote(envFilePath, lineNumber, value);
	std::string trailing = trim(value.substr(closingQuote + 1));
	if (!trailing.empty() && trailing.front() != '#')
	{
		throw makeParseError(
			envFilePath, lineNumber, "Unexpected text after quoted value.");
	}
	std::string_view quotedBody = value.substr(1, closingQuote - 1);
	if (quote == '"')
	{
		return decodeDoubleQuotedValue(quotedBody);
	}
	return std::string(quotedBody);
}

std::string parseValue(
	const std::filesystem::path &envFilePath,
	std::size_t lineNumber,
	std::string_view rawValue)
{
	std::string value = trim(rawValue);
	if (value.empty())
	{
		return {};
	}
	if (value.front() == '\'' || value.front() == '"')
	{
		return parseQuotedValue(envFilePath, lineNumber, value);
	}
	return trim(value.substr(0, value.find('#')));
}

std::pair<std::string, std::string> parseAssignment(
	const std::filesystem::path &envFilePath,
	std::size_t lineNumber,
	const std::string &line)
{
	std::size_t separator = line.find('=');
	if (separator == std::string::npos)
	{
		throw makeParseError(envFilePath, lineNumber, "Expected KEY=value.");
	}

	std::string name = trim(std::string_view(line).substr(0, separator));
	if (!nameIsValid(name))
	{
		throw makeParseError(envFilePath, lineNumber, "Invalid variable name.");
	}

	return {
		std::move(name),
		parseValue(
			envFilePath,
			lineNumber,
			std::string_view(line).substr(separator + 1))};
}

} // namespace

EnvKvStore::EnvKvStore(
	const std::vector<std::filesystem::path> &envFilePaths,
	std::ostream &warningStream)
{
	loadFiles(envFilePaths, warningStream);
}

EnvKvStore::EnvKvStore(
	const std::vector<std::filesystem::path> &envFilePaths)
:	EnvKvStore(envFilePaths, std::cerr)
{
}

void EnvKvStore::loadFiles(
	const std::vector<std::filesystem::path> &envFilePaths,
	std::ostream &warningStream)
{
	for (const std::filesystem::path &envFilePath : envFilePaths)
	{
		loadFile(envFilePath, warningStream);
	}
}

std::optional<std::string> EnvKvStore::get(std::string_view name) const
{
	std::string ownedName(name);
	if (const char *value = std::getenv(ownedName.c_str()))
	{
		return std::string(value);
	}
	auto value = values.find(std::string(name));
	if (value == values.end())
	{
		return std::nullopt;
	}
	return value->second;
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
		if (lineIsBlankOrComment(line))
		{
			continue;
		}
		auto [name, value] = parseAssignment(envFilePath, lineNumber, line);
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
