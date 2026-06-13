#ifndef SPINSCALE_TEST_SUPPORT_GROUP_ASSERTIONS_H
#define SPINSCALE_TEST_SUPPORT_GROUP_ASSERTIONS_H

#include <exception>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <spinscale/co/group.h>

namespace sscl::tests {

template <typename Invoker>
int completedIntValue(Invoker &invoker)
{
	if (invoker.completedReturnValues().myExceptionPtr) {
		std::rethrow_exception(
			invoker.completedReturnValues().myExceptionPtr);
	}

	return invoker.completedReturnValues().myReturnValue;
}

inline void requireCompletedSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor)
{
	if (descriptor.type !=
		sscl::co::Group::SettlementDescriptor::TypeE::COMPLETED)
	{
		throw std::runtime_error("Expected completed settlement");
	}
}

template <typename Invoker>
void requireCompletedIntSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	int expectedValue)
{
	requireCompletedSettlement(descriptor);

	const int actualValue = completedIntValue(descriptor.invokerAs<Invoker>());
	if (actualValue != expectedValue) {
		throw std::runtime_error(
			"Expected completed settlement value "
			+ std::to_string(expectedValue)
			+ ", got "
			+ std::to_string(actualValue));
	}
}

template <typename Invoker>
void expectCompletedIntSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	int expectedValue)
{
	EXPECT_NO_THROW(
		requireCompletedIntSettlement<Invoker>(
			descriptor,
			expectedValue));
}

inline void expectCompletedSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor)
{
	EXPECT_NO_THROW(requireCompletedSettlement(descriptor));
}

inline void requireExceptionSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor)
{
	if (descriptor.type !=
		sscl::co::Group::SettlementDescriptor::TypeE::EXCEPTION_THROWN)
	{
		throw std::runtime_error("Expected exception settlement");
	}

	if (!descriptor.calleeException) {
		throw std::runtime_error("Expected exception pointer in settlement");
	}
}

inline void expectExceptionSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor)
{
	EXPECT_NO_THROW(requireExceptionSettlement(descriptor));
}

inline void requireRuntimeErrorSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	const std::string &expectedMessage)
{
	requireExceptionSettlement(descriptor);

	try {
		std::rethrow_exception(descriptor.calleeException);
	}
	catch (const std::runtime_error &runtimeError) {
		const std::string actualMessage = runtimeError.what();
		if (actualMessage != expectedMessage) {
			throw std::runtime_error(
				"Expected runtime_error settlement message \""
				+ expectedMessage
				+ "\", got \""
				+ actualMessage
				+ "\"");
		}
		return;
	}
	catch (...) {
		throw std::runtime_error("Expected std::runtime_error settlement");
	}
}

inline void requireIntExceptionSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	int expectedValue)
{
	requireExceptionSettlement(descriptor);

	try {
		std::rethrow_exception(descriptor.calleeException);
	}
	catch (int caughtValue) {
		if (caughtValue != expectedValue) {
			throw std::runtime_error(
				"Expected int exception settlement value "
				+ std::to_string(expectedValue)
				+ ", got "
				+ std::to_string(caughtValue));
		}
		return;
	}
	catch (...) {
		throw std::runtime_error("Expected int exception settlement");
	}
}

inline void expectIntExceptionSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	int expectedValue)
{
	EXPECT_NO_THROW(
		requireIntExceptionSettlement(
			descriptor,
			expectedValue));
}

inline void expectRuntimeErrorSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	const std::string &expectedMessage)
{
	EXPECT_NO_THROW(
		requireRuntimeErrorSettlement(
			descriptor,
			expectedMessage));
}

inline void requireEmptyGroupError(
	const std::runtime_error &runtimeError)
{
	constexpr const char *expectedMessage =
		"co_await: Group has no member invokers; call add() before awaiting";
	if (std::string(runtimeError.what()) != expectedMessage) {
		throw std::runtime_error("Unexpected empty group error message");
	}
}

inline void expectEmptyGroupError(
	const std::runtime_error &runtimeError)
{
	EXPECT_NO_THROW(requireEmptyGroupError(runtimeError));
}

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_GROUP_ASSERTIONS_H
