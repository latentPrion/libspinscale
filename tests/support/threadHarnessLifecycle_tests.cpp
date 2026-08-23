#include <boostAsioLinkageFix.h>

#include <gtest/gtest.h>
#include <support/threadHarness.h>

namespace sscl::tests {
namespace {

constexpr unsigned postingThreadSetLifecycleIterationCount = 100u;

void doNothing()
{}

void exerciseAllPostingThreads(PostingThreadSet& threads)
{
	threads.caller().runSync(doNothing);
	threads.callee().runSync(doNothing);
	threads.alternate().runSync(doNothing);
	threads.body().runSync(doNothing);
	threads.world().runSync(doNothing);
	threads.leg().runSync(doNothing);
}

TEST(PostingThreadSetLifecycleTest, RepeatedConstructionAndDestructionDoesNotHang)
{
	for (unsigned iteration = 0u;
		iteration < postingThreadSetLifecycleIterationCount;
		++iteration)
	{
		PostingThreadSet threads;
		exerciseAllPostingThreads(threads);
	}
}

} // namespace
} // namespace sscl::tests
