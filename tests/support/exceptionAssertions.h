#ifndef SPINSCALE_TEST_SUPPORT_EXCEPTION_ASSERTIONS_H
#define SPINSCALE_TEST_SUPPORT_EXCEPTION_ASSERTIONS_H

#include <exception>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace sscl::tests {

inline void requireExceptionMessageContains(
	const std::exception &exception,
	const std::string &expectedSubstring)
{
	const std::string message = exception.what();
	if (message.find(expectedSubstring) == std::string::npos) {
		throw std::runtime_error(
			"Expected exception message to contain \""
			+ expectedSubstring
			+ "\", got \""
			+ message
			+ "\"");
	}
}

inline void expectExceptionMessageContains(
	const std::exception &exception,
	const std::string &expectedSubstring)
{
	EXPECT_NO_THROW(
		requireExceptionMessageContains(exception, expectedSubstring));
}

inline void requireExceptionPtrMessageContains(
	const std::exception_ptr &exceptionPtr,
	const std::string &expectedSubstring)
{
	try {
		std::rethrow_exception(exceptionPtr);
	}
	catch (const std::exception &exception) {
		requireExceptionMessageContains(exception, expectedSubstring);
		return;
	}
	catch (...) {
		throw std::runtime_error("Expected std::exception in exception_ptr");
	}
}

inline void expectExceptionPtrMessageContains(
	const std::exception_ptr &exceptionPtr,
	const std::string &expectedSubstring)
{
	EXPECT_NO_THROW(
		requireExceptionPtrMessageContains(
			exceptionPtr,
			expectedSubstring));
}

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_EXCEPTION_ASSERTIONS_H
