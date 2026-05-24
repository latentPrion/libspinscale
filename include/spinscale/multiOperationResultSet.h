#ifndef MULTI_OPERATION_RESULT_SET_H
#define MULTI_OPERATION_RESULT_SET_H

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

} // namespace sscl

#endif // MULTI_OPERATION_RESULT_SET_H
