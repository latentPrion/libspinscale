#include <boostAsioLinkageFix.h>

#include <spinscale/multiOperationResultSet.h>
#include <spinscale/co/group.h>

namespace sscl {

MultiOperationResultSetWithException::MultiOperationResultSetWithException(
	const co::Group &group)
{
	unsigned int nSucceeded = 0;
	unsigned int nFailed = 0;
	using SettlementType = co::Group::SettlementDescriptor::TypeE;

	for (const auto &desc : group.s.rsrc.settlements)
	{
		if (desc.type == SettlementType::EXCEPTION_THROWN) {
			nFailed++;
		}
		else {
			nSucceeded++;
		}
	}

	results = MultiOperationResultSet(
		static_cast<unsigned int>(group.s.rsrc.settlements.size()),
		nSucceeded,
		nFailed);

	if (nFailed > 0) {
		memberFailureException = group.captureAggregatedGroupExceptions();
	}
}

} // namespace sscl
