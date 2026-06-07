#ifndef MULTI_OPERATION_RESULT_SET_H
#define MULTI_OPERATION_RESULT_SET_H

#include <exception>

namespace sscl {

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

	bool hasMemberFailure() const
		{ return memberFailureException != nullptr; }

	MultiOperationResultSet results;
	std::exception_ptr memberFailureException = nullptr;
};

} // namespace sscl

#endif // MULTI_OPERATION_RESULT_SET_H
