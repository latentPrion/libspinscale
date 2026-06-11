#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <spinscale/envKvStore.h>

namespace {

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
		unsetenv("SSCL_ENV_TEST_VALUE");
	}

	void TearDown() override
	{
		unsetenv("SSCL_ENV_TEST_VALUE");
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

	EXPECT_EQ(store.get("PLAIN"), "value");
	EXPECT_EQ(store.get("TRIMMED"), "value with spaces");
	EXPECT_EQ(store.get("SINGLE"), " preserved value ");
	EXPECT_EQ(store.get("DOUBLE"), "another preserved value");
	EXPECT_EQ(store.get("ESCAPED"), "quote: \" slash: \\ tab: \t");
	EXPECT_EQ(store.get("COMMENTED"), "value");
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, LaterFilesOverwriteEarlierFilesAndWarn)
{
	std::filesystem::path first = writeFile("first.env", "VALUE=first\n");
	std::filesystem::path second = writeFile("second.env", "VALUE=second\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({first, second}, warnings);

	EXPECT_EQ(store.get("VALUE"), "second");
	EXPECT_NE(warnings.str().find("VALUE"), std::string::npos);
	EXPECT_NE(warnings.str().find("first"), std::string::npos);
	EXPECT_NE(warnings.str().find("second"), std::string::npos);
	EXPECT_NE(warnings.str().find(second.string()), std::string::npos);
}

TEST_F(EnvKvStoreTest, DuplicateKeysInsideSameFileOverwriteAndWarn)
{
	std::filesystem::path envFile =
		writeFile("one.env", "VALUE=first\nVALUE=second\n");

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.get("VALUE"), "second");
	EXPECT_NE(warnings.str().find("VALUE"), std::string::npos);
	EXPECT_NE(warnings.str().find("first"), std::string::npos);
	EXPECT_NE(warnings.str().find("second"), std::string::npos);
	EXPECT_NE(warnings.str().find(envFile.string()), std::string::npos);
}

TEST_F(EnvKvStoreTest, ProcessEnvironmentOverridesStoreSilently)
{
	std::filesystem::path envFile =
		writeFile("one.env", "SSCL_ENV_TEST_VALUE=file\n");
	setenv("SSCL_ENV_TEST_VALUE", "process", 1);

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.get("SSCL_ENV_TEST_VALUE"), "process");
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, EmptyProcessEnvironmentValueOverridesStoreSilently)
{
	std::filesystem::path envFile =
		writeFile("one.env", "SSCL_ENV_TEST_VALUE=file\n");
	setenv("SSCL_ENV_TEST_VALUE", "", 1);

	std::ostringstream warnings;
	sscl::EnvKvStore store({envFile}, warnings);

	EXPECT_EQ(store.get("SSCL_ENV_TEST_VALUE"), "");
	EXPECT_TRUE(warnings.str().empty());
}

TEST_F(EnvKvStoreTest, MissingFileThrows)
{
	std::ostringstream warnings;
	EXPECT_THROW(
		sscl::EnvKvStore({root / "missing.env"}, warnings),
		std::runtime_error);
}

TEST_F(EnvKvStoreTest, MalformedLineThrows)
{
	std::filesystem::path envFile = writeFile("bad.env", "NOT AN ASSIGNMENT\n");
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
		EXPECT_NE(message.find(":1:"), std::string::npos);
	}
}
