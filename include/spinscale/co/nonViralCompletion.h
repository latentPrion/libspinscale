#ifndef NON_VIRAL_COMPLETION_H
#define NON_VIRAL_COMPLETION_H

#include <exception>
#include <utility>

namespace sscl::co {

class NonViralCompletion
{
public:
	explicit NonViralCompletion(std::exception_ptr &exceptionPtr)
	: exceptionPtr(exceptionPtr)
	{}

	bool hasException() const noexcept
	{
		return exceptionPtr != nullptr;
	}

	void checkAndRethrowException() const
	{
		if (exceptionPtr)
		{
			std::rethrow_exception(exceptionPtr);
		}
	}

	std::exception_ptr releaseException() noexcept
	{
		return std::exchange(exceptionPtr, nullptr);
	}

private:
	std::exception_ptr &exceptionPtr;
};

} // namespace sscl::co

#endif // NON_VIRAL_COMPLETION_H
