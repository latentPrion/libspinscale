#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <spinscale/envKvStore.h>

namespace {

constexpr const char *kTestEnvName = "SSCL_ENV_TEST_VALUE";
constexpr const char *kPositiveIntMsEnvName = "SSCL_POSITIVE_INT_MS";
constexpr int kPositiveIntMsDefault = 33;

void unsetTestEnvVars()
{
	unsetenv(kTestEnvName);
	unsetenv(kPositiveIntMsEnvName);
}

class EnvKvStoreTest
:	public testing::Test
{
protected:
	void SetUp() override
	{
		root = std::filesystem::temp_directory_path()
			/ ("spinscale-env-test-"
				+ std::to_string(std::chrono::steady_clock::now()
					.time_since_epoch().count())
				+ "-" + std::to_string(testCounter++));
		std::filesystem::create_directories(root);
		unsetTestEnvVars();
	}

	void TearDown() override
	{
		unsetTestEnvVars();
		std::filesystem::remove_all(root);
	}

	std::filesystem::path writeFile(
		const std::string &filename,
		const std::string &contents)
	{
		std::filesystem::path path = root / filename;
		std::ofstream file(path);
		file << contents;
		return path;
	}

	void expectParseErrorContaining(
		const std::filesystem::path &envFile,
		const std::string &expectedFragment)
	{
		std::ostringstream warnings;
		try
		{
			sscl::EnvKvStore store({envFile}, warnings);
			FAIL() << "Expected parse of " << envFile << " to throw.";
		}
		catch (const std::runtime_error &e)
		{
			std::string message = e.what();
			EXPECT_NE(message.find(envFile.string()), std::string::npos)
				<< message;
			EXPECT_NE(message.find(expectedFragment), std::string::npos)
				<< message;
		}
	}

	std::filesystem::path root;
	static inline int testCounter = 0;
};

} // namespace

TEST_F(EnvKvStoreTest, ParsesSupportedDotenvForms)
{
	std::filesystem::path envFile = writeFile(
		"one.env",
		"\n"
		"# comment\n"
		"PLAIN=value\n"
		" TRIMMED = value with spaces   \n"
		"SINGLE=' preserved value '\n"
		"DOUBLE=\"another preserved value\"\n"
		"ESCAPED=\"quote: \\\" slash: \\\\ tab: \\t\"\n"
		"COMMENTED=value # comment\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("PLAIN"), "value");
	EXPECT_EQ(store.find("TRIMMED"), "value with spaces");
	EXPECT_EQ(store.find("SINGLE"), " preserved value ");
	EXPECT_EQ(store.find("DOUBLE"), "another preserved value");
	EXPECT_EQ(store.find("ESCAPED"), "quote: \" slash: \\ tab: \t");
	EXPECT_EQ(store.find("COMMENTED"), "value");
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, EmptyPathListYieldsEmptyStore)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store({}, warnings);

	EXPECT_EQ(store.find("ANY"), std::nullopt);
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, EmptyFileYieldsEmptyStore)
{
	std::filesystem::path envFile = writeFile("empty.env", "");
	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("ANY"), std::nullopt);
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, CommentOnlyAndWhitespaceOnlyLinesAreIgnored)
{
	std::filesystem::path envFile = writeFile(
		"comments.env",
		"   \n"
		"\t\n"
		"# only comment\n"
		"  # indented comment\n"
		"KEEP=yes\n"
		"\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("KEEP"), "yes");
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, EmptyUnquotedValueIsAccepted)
{
	std::filesystem::path envFile = writeFile("empty-value.env", "EMPTY=\n");
	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("EMPTY"), "");
}

TEST_F(EnvKvStoreTest, UnquotedHashStartsInlineComment)
{
	std::filesystem::path envFile = writeFile(
		"hash.env",
		"A=before#after\n"
		"B= # leading comment after equals\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("A"), "before");
	EXPECT_EQ(store.find("B"), "");
}

TEST_F(EnvKvStoreTest, HashInsideQuotesIsLiteral)
{
	std::filesystem::path envFile = writeFile(
		"hash-quoted.env",
		"SINGLE='#not-comment'\n"
		"DOUBLE=\"#not-comment\"\n"
		"DOUBLE_TRAIL=\"kept\" # trailing comment ok\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("SINGLE"), "#not-comment");
	EXPECT_EQ(store.find("DOUBLE"), "#not-comment");
	EXPECT_EQ(store.find("DOUBLE_TRAIL"), "kept");
}

TEST_F(EnvKvStoreTest, DoubleQuotedEscapeSequences)
{
	std::filesystem::path envFile = writeFile(
		"escapes.env",
		"NL=\"line\\nbreak\"\n"
		"CR=\"ret\\rurn\"\n"
		"TAB=\"a\\tb\"\n"
		"UNKNOWN=\"\\q\"\n"
		"TRAILING=\"end\\\\\"\n"
		"ESCAPED_QUOTE_MID=\"a\\\"b\"\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("NL"), "line\nbreak");
	EXPECT_EQ(store.find("CR"), "ret\rurn");
	EXPECT_EQ(store.find("TAB"), "a\tb");
	EXPECT_EQ(store.find("UNKNOWN"), "q");
	EXPECT_EQ(store.find("TRAILING"), "end\\");
	EXPECT_EQ(store.find("ESCAPED_QUOTE_MID"), "a\"b");
}

TEST_F(EnvKvStoreTest, SingleQuotedValuesDoNotDecodeEscapes)
{
	std::filesystem::path envFile = writeFile(
		"single-escapes.env",
		"LITERAL='\\n\\t\\\"'\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("LITERAL"), "\\n\\t\\\"");
}

TEST_F(EnvKvStoreTest, ValidNamesAcceptUnderscoreAndAlnum)
{
	std::filesystem::path envFile = writeFile(
		"names.env",
		"_LEADING=1\n"
		"A1B2=2\n"
		"mixed_Case99=3\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("_LEADING"), "1");
	EXPECT_EQ(store.find("A1B2"), "2");
	EXPECT_EQ(store.find("mixed_Case99"), "3");
}

TEST_F(EnvKvStoreTest, InvalidNamesThrow)
{
	expectParseErrorContaining(
		writeFile("digit.env", "1BAD=x\n"),
		"Invalid variable name.");
	expectParseErrorContaining(
		writeFile("hyphen.env", "BAD-NAME=x\n"),
		"Invalid variable name.");
	expectParseErrorContaining(
		writeFile("dot.env", "BAD.NAME=x\n"),
		"Invalid variable name.");
	expectParseErrorContaining(
		writeFile("empty-name.env", "=value\n"),
		"Invalid variable name.");
}

TEST_F(EnvKvStoreTest, UnterminatedQuotedValueThrows)
{
	expectParseErrorContaining(
		writeFile("unterminated-double.env", "X=\"no close\n"),
		"Unterminated quoted value.");
	expectParseErrorContaining(
		writeFile("unterminated-single.env", "X='no close\n"),
		"Unterminated quoted value.");
}

TEST_F(EnvKvStoreTest, UnexpectedTextAfterQuotedValueThrows)
{
	expectParseErrorContaining(
		writeFile("trail.env", "X=\"ok\" trailing\n"),
		"Unexpected text after quoted value.");
}

TEST_F(EnvKvStoreTest, MalformedLineThrowsWithLineNumber)
{
	std::filesystem::path envFile = writeFile(
		"bad.env",
		"# header\n"
		"GOOD=1\n"
		"NOT AN ASSIGNMENT\n");

	std::ostringstream warnings;
	try
	{
		sscl::EnvKvStore store({envFile}, warnings);
		FAIL() << "Expected malformed env file to throw.";
	}
	catch (const std::runtime_error &e)
	{
		std::string message = e.what();
		EXPECT_NE(message.find(envFile.string()), std::string::npos);
		EXPECT_NE(message.find(":3:"), std::string::npos) << message;
		EXPECT_NE(message.find("Expected KEY=value."), std::string::npos)
			<< message;
	}
}

TEST_F(EnvKvStoreTest, MissingFileThrows)
{
	std::ostringstream warnings;
	EXPECT_THROW(
		sscl::EnvKvStore({root / "missing.env"}, warnings),
		std::runtime_error);
}

TEST_F(EnvKvStoreTest, UnreadableFileThrowsOpenFailure)
{
	std::filesystem::path envFile = writeFile("noread.env", "X=1\n");
	std::filesystem::permissions(envFile, std::filesystem::perms::none);

	std::ostringstream warnings;
	try
	{
		sscl::EnvKvStore store({envFile}, warnings);
		FAIL() << "Expected unreadable file open to throw.";
	}
	catch (const std::runtime_error &e)
	{
		EXPECT_NE(
			std::string(e.what()).find("Failed to open env file:"),
			std::string::npos);
	}

	std::filesystem::permissions(
		envFile,
		std::filesystem::perms::owner_read
			| std::filesystem::perms::owner_write);
}

TEST_F(EnvKvStoreTest, LaterFilesOverwriteEarlierFilesAndWarn)
{
	std::filesystem::path first = writeFile("first.env", "VALUE=first\n");
	std::filesystem::path second = writeFile("second.env", "VALUE=second\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({first, second}, warnings);

	EXPECT_EQ(store.find("VALUE"), "second");
	EXPECT_NE(warnings.str().find("VALUE"), std::string::npos);
	EXPECT_NE(warnings.str().find("first"), std::string::npos);
	EXPECT_NE(warnings.str().find("second"), std::string::npos);
	EXPECT_NE(warnings.str().find(second.string()), std::string::npos);
}

TEST_F(EnvKvStoreTest, ThreeFileOverwriteKeepsLastValue)
{
	std::filesystem::path a = writeFile("a.env", "K=a\nSHARED=1\n");
	std::filesystem::path b = writeFile("b.env", "SHARED=2\n");
	std::filesystem::path c = writeFile("c.env", "SHARED=3\nK=c\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({a, b, c}, warnings);

	EXPECT_EQ(store.find("K"), "c");
	EXPECT_EQ(store.find("SHARED"), "3");
	const std::string warningText = warnings.str();
	EXPECT_GE(
		std::count(warningText.begin(), warningText.end(), '\n'),
		2);
}

TEST_F(EnvKvStoreTest, DuplicateKeysInsideSameFileOverwriteAndWarn)
{
	std::filesystem::path envFile =
		writeFile("one.env", "VALUE=first\nVALUE=second\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("VALUE"), "second");
	EXPECT_NE(warnings.str().find("VALUE"), std::string::npos);
	EXPECT_NE(warnings.str().find("first"), std::string::npos);
	EXPECT_NE(warnings.str().find("second"), std::string::npos);
	EXPECT_NE(warnings.str().find(envFile.string()), std::string::npos);
}

TEST_F(EnvKvStoreTest, ProcessEnvironmentOverridesStoreSilently)
{
	std::filesystem::path envFile =
		writeFile("one.env", "SSCL_ENV_TEST_VALUE=file\n");
	setenv(kTestEnvName, "process", 1);

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find(kTestEnvName), "process");
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, EmptyProcessEnvironmentValueOverridesStoreSilently)
{
	std::filesystem::path envFile =
		writeFile("one.env", "SSCL_ENV_TEST_VALUE=file\n");
	setenv(kTestEnvName, "", 1);

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find(kTestEnvName), "");
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, ProcessEnvironmentReturnedWhenKeyAbsentFromStore)
{
	setenv(kTestEnvName, "only-process", 1);
	std::ostringstream warnings;
	sscl::EnvKvStore store({}, warnings);

	EXPECT_EQ(store.find(kTestEnvName), "only-process");
}

TEST_F(EnvKvStoreTest, BypassProcessEnvironmentUsesFileStoreOnly)
{
	std::filesystem::path envFile =
		writeFile("one.env", "SSCL_ENV_TEST_VALUE=file\n");
	setenv(kTestEnvName, "process", 1);

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find(kTestEnvName, true), "file");
	EXPECT_EQ(store.find(kTestEnvName, false), "process");
}

TEST_F(EnvKvStoreTest, BypassIgnoresProcessEnvWhenKeyOnlyInProcess)
{
	setenv(kTestEnvName, "process-only", 1);
	std::ostringstream warnings;
	sscl::EnvKvStore store({}, warnings);

	EXPECT_EQ(store.find(kTestEnvName, true), std::nullopt);
	EXPECT_EQ(store.find(kTestEnvName, false), "process-only");
}

TEST_F(EnvKvStoreTest, MissingKeyReturnsNullopt)
{
	std::filesystem::path envFile = writeFile("one.env", "PRESENT=1\n");
	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.find("PRESENT"), "1");
	EXPECT_EQ(store.find("ABSENT"), std::nullopt);
	EXPECT_EQ(store.find("ABSENT", true), std::nullopt);
}

TEST_F(EnvKvStoreTest, DefaultWarningCtorLoadsWithoutThrowing)
{
	std::filesystem::path envFile = writeFile("one.env", "OK=1\n");
	sscl::EnvKvStore store({envFile});
	EXPECT_EQ(store.find("OK"), "1");
}

TEST_F(EnvKvStoreTest, GetThrowsWhenKeyMissing)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store({}, warnings);
	EXPECT_THROW(store.get("ABSENT"), std::runtime_error);
}

TEST_F(EnvKvStoreTest, GetReturnsPresentValue)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store(
		{writeFile("one.env", "PRESENT=1\n")},
		warnings);
	EXPECT_EQ(store.get("PRESENT"), "1");
}

TEST_F(EnvKvStoreTest, GetIntUsesDefaultWhenUnset)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store({}, warnings);

	EXPECT_EQ(store.getInt(kPositiveIntMsEnvName, kPositiveIntMsDefault), kPositiveIntMsDefault);
	EXPECT_EQ(store.getInt("CUSTOM", 42), 42);
	EXPECT_THROW(store.getInt("MISSING"), std::runtime_error);
}

TEST_F(EnvKvStoreTest, GetIntParsesSignedValues)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store(
		{writeFile("one.env", "NEG=-7\nZERO=0\n")},
		warnings);
	EXPECT_EQ(store.getInt("NEG"), -7);
	EXPECT_EQ(store.getInt("ZERO"), 0);
	EXPECT_EQ(store.getPositiveInt("ZERO"), 0);
}

TEST_F(EnvKvStoreTest, GetPositiveNonZeroIntUsesDefaultWhenUnset)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store({}, warnings);

	EXPECT_EQ(
		store.getPositiveNonZeroInt(kPositiveIntMsEnvName, kPositiveIntMsDefault),
		kPositiveIntMsDefault);
}

TEST_F(EnvKvStoreTest, GetPositiveNonZeroIntParsesFileValue)
{
	std::filesystem::path envFile = writeFile(
		"one.env",
		std::string(kPositiveIntMsEnvName) + "=50\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(
		store.getPositiveNonZeroInt(kPositiveIntMsEnvName, kPositiveIntMsDefault),
		50);
}

TEST_F(EnvKvStoreTest, GetPositiveNonZeroIntHonorsProcessEnvironmentOverFile)
{
	std::filesystem::path envFile = writeFile(
		"one.env",
		std::string(kPositiveIntMsEnvName) + "=50\n");
	setenv(kPositiveIntMsEnvName, "77", 1);

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(
		store.getPositiveNonZeroInt(kPositiveIntMsEnvName, kPositiveIntMsDefault),
		77);
}

TEST_F(EnvKvStoreTest, GetPositiveNonZeroIntRejectsNonPositive)
{
	std::ostringstream warnings;
	sscl::EnvKvStore zeroStore(
		{writeFile(
			"zero.env",
			std::string(kPositiveIntMsEnvName) + "=0\n")},
		warnings);
	EXPECT_THROW(
		zeroStore.getPositiveNonZeroInt(
			kPositiveIntMsEnvName, kPositiveIntMsDefault),
		std::runtime_error);

	sscl::EnvKvStore negativeStore(
		{writeFile(
			"neg.env",
			std::string(kPositiveIntMsEnvName) + "=-3\n")},
		warnings);
	EXPECT_THROW(
		negativeStore.getPositiveNonZeroInt(
			kPositiveIntMsEnvName, kPositiveIntMsDefault),
		std::runtime_error);
	EXPECT_THROW(
		negativeStore.getPositiveInt(
			kPositiveIntMsEnvName, kPositiveIntMsDefault),
		std::runtime_error);
}

TEST_F(EnvKvStoreTest, GetPositiveNonZeroIntRejectsNonNumericAndTrailingJunk)
{
	std::ostringstream warnings;

	sscl::EnvKvStore letters(
		{writeFile(
			"letters.env",
			std::string(kPositiveIntMsEnvName) + "=abc\n")},
		warnings);
	try
	{
		(void)letters.getPositiveNonZeroInt(
			kPositiveIntMsEnvName, kPositiveIntMsDefault);
		FAIL() << "Expected non-numeric parse to throw.";
	}
	catch (const std::runtime_error &e)
	{
		EXPECT_NE(
			std::string(e.what()).find("failed to parse"),
			std::string::npos)
			<< e.what();
	}

	sscl::EnvKvStore trailing(
		{writeFile(
			"trailing.env",
			std::string(kPositiveIntMsEnvName) + "=50ms\n")},
		warnings);
	try
	{
		(void)trailing.getPositiveNonZeroInt(
			kPositiveIntMsEnvName, kPositiveIntMsDefault);
		FAIL() << "Expected trailing junk to throw.";
	}
	catch (const std::runtime_error &e)
	{
		EXPECT_NE(
			std::string(e.what()).find("must be an integer"),
			std::string::npos)
			<< e.what();
	}
}

TEST_F(EnvKvStoreTest, GetPositiveNonZeroIntRejectsEmptyStringValue)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store(
		{writeFile(
			"empty.env",
			std::string(kPositiveIntMsEnvName) + "=\n")},
		warnings);

	EXPECT_THROW(
		store.getPositiveNonZeroInt(kPositiveIntMsEnvName, kPositiveIntMsDefault),
		std::runtime_error);
}

TEST_F(EnvKvStoreTest, GetPositiveNonZeroIntAcceptsOne)
{
	std::ostringstream warnings;
	sscl::EnvKvStore store(
		{writeFile("one.env", "CUSTOM=1\n")},
		warnings);

	EXPECT_EQ(store.getPositiveNonZeroInt("CUSTOM", 99), 1);
}
