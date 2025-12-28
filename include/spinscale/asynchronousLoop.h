#ifndef ASYNCHRONOUS_LOOP_H
#define ASYNCHRONOUS_LOOP_H

#include <atomic>

namespace sscl {

class AsynchronousLoop
{
public:
	AsynchronousLoop(
		const unsigned int nTotal,
		unsigned int nSucceeded=0, unsigned int nFailed=0)
	: nTotal(nTotal), nSucceeded(nSucceeded), nFailed(nFailed)
	{}

	AsynchronousLoop(const AsynchronousLoop& other)
	: nTotal(other.nTotal),
	nSucceeded(other.nSucceeded.load()), nFailed(other.nFailed.load())
	{}

	AsynchronousLoop& operator=(const AsynchronousLoop& other)
	{
		if (this != &other)
		{
			nTotal = other.nTotal;
			nSucceeded.store(other.nSucceeded.load());
			nFailed.store(other.nFailed.load());
		}
		return *this;
	}

	bool isComplete(void) const
	{
		return nSucceeded + nFailed == nTotal;
	}

	void incrementSuccessOrFailureDueTo(bool success)
	{
		if (success)
			{ ++nSucceeded; }
		else
			{ ++nFailed; }
	}

	bool incrementSuccessOrFailureAndTestForCompletionDueTo(bool success)
	{
		incrementSuccessOrFailureDueTo(success);
		return isComplete();
	}

	bool nTotalIsZero(void) const
	{
		return nTotal == 0;
	}

	void setRemainingIterationsToFailure()
	{
		nFailed.store(nTotal - nSucceeded.load());
	}

public:
	unsigned int nTotal;
	std::atomic<unsigned int> nSucceeded, nFailed;
};

} // namespace sscl

#endif // ASYNCHRONOUS_LOOP_H
