#ifndef MULTI_OPERATION_RESULT_SET_H
#define MULTI_OPERATION_RESULT_SET_H

#include <exception>

namespace sscl {
namespace co {
struct Group;
} // namespace co

/** Plain aggregate for fan-out / fan-in results returned from coroutines. */
struct MultiOperationResultSet
{
	MultiOperationResultSet(
		unsigned int total = 0,
		unsigned int succeeded = 0,
		unsigned int failed = 0)
	: nTotal(total), nSucceeded(succeeded), nFailed(failed)
	{}

	bool isComplete() const
		{ return nSucceeded + nFailed == nTotal; }

	bool nTotalIsZero() const
		{ return nTotal == 0; }

	unsigned int nTotal;
	unsigned int nSucceeded;
	unsigned int nFailed;
};

/** Fan-out / fan-in counts plus optional aggregated member failure. */
struct MultiOperationResultSetWithException
{
	MultiOperationResultSetWithException() = default;

	MultiOperationResultSetWithException(
		MultiOperationResultSet resultsIn,
		std::exception_ptr memberFailureExceptionIn = nullptr)
	: results(resultsIn),
	  memberFailureException(memberFailureExceptionIn)
	{}

	/** Summarize a settled Group into counts + aggregated member failure. */
	explicit MultiOperationResultSetWithException(const co::Group &group);

	bool hasMemberFailure() const
		{ return memberFailureException != nullptr; }

	/** Combine this result set with another phase's counts and exception. */
	MultiOperationResultSetWithException mergeWith(
		const MultiOperationResultSetWithException &other) const
	{
		std::exception_ptr memberFailure = memberFailureException;
		if (!memberFailure && other.hasMemberFailure()) {
			memberFailure = other.memberFailureException;
		}

		return MultiOperationResultSetWithException(
			MultiOperationResultSet(
				results.nTotal + other.results.nTotal,
				results.nSucceeded + other.results.nSucceeded,
				results.nFailed + other.results.nFailed),
			memberFailure);
	}

	MultiOperationResultSet results;
	std::exception_ptr memberFailureException = nullptr;
};

} // namespace sscl

#endif // MULTI_OPERATION_RESULT_SET_H
