#ifndef SPINSCALE_TEST_SUPPORT_GROUP_ASSERTIONS_H
#define SPINSCALE_TEST_SUPPORT_GROUP_ASSERTIONS_H

#include <exception>
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

inline void expectCompletedSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor)
{
	EXPECT_EQ(
		descriptor.type,
		sscl::co::Group::SettlementDescriptor::TypeE::COMPLETED);
}

template <typename Invoker>
void expectCompletedIntSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	int expectedValue)
{
	ASSERT_EQ(
		descriptor.type,
		sscl::co::Group::SettlementDescriptor::TypeE::COMPLETED);
	EXPECT_EQ(completedIntValue(descriptor.invokerAs<Invoker>()), expectedValue);
}

inline void expectExceptionSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor)
{
	EXPECT_EQ(
		descriptor.type,
		sscl::co::Group::SettlementDescriptor::TypeE::EXCEPTION_THROWN);
	EXPECT_TRUE(descriptor.calleeException != nullptr);
}

inline void expectRuntimeErrorSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	const std::string &expectedMessage)
{
	ASSERT_EQ(
		descriptor.type,
		sscl::co::Group::SettlementDescriptor::TypeE::EXCEPTION_THROWN);
	ASSERT_TRUE(descriptor.calleeException != nullptr);

	try {
		std::rethrow_exception(descriptor.calleeException);
	}
	catch (const std::runtime_error &runtimeError) {
		EXPECT_EQ(std::string(runtimeError.what()), expectedMessage);
		return;
	}
	catch (...) {
		FAIL() << "Expected std::runtime_error settlement.";
	}
}

inline void expectIntExceptionSettlement(
	const sscl::co::Group::SettlementDescriptor &descriptor,
	int expectedValue)
{
	ASSERT_EQ(
		descriptor.type,
		sscl::co::Group::SettlementDescriptor::TypeE::EXCEPTION_THROWN);
	ASSERT_TRUE(descriptor.calleeException != nullptr);

	try {
		std::rethrow_exception(descriptor.calleeException);
	}
	catch (int caughtValue) {
		EXPECT_EQ(caughtValue, expectedValue);
		return;
	}
	catch (...) {
		FAIL() << "Expected int exception settlement.";
	}
}

inline void expectEmptyGroupError(
	const std::runtime_error &runtimeError)
{
	constexpr const char *expectedMessage =
		"co_await: Group has no member invokers; call add() before awaiting";
	EXPECT_EQ(std::string(runtimeError.what()), expectedMessage);
}

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_GROUP_ASSERTIONS_H
