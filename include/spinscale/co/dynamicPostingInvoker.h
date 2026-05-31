#ifndef DYNAMIC_POSTING_INVOKER_H
#define DYNAMIC_POSTING_INVOKER_H

#include <stdexcept>

#include <boost/asio/io_context.hpp>

#include <spinscale/co/invokers.h>
#include <spinscale/co/postingPromise.h>

namespace sscl::co {

/**	Fallback ThreadTag for DynamicViralPostingInvoker when ExplicitPostTarget is
 *	omitted. Callers must always pass ExplicitPostTarget in production paths.
 */
struct DynamicPostTargetThreadTag
{
	static boost::asio::io_context &io_context()
	{
		throw std::runtime_error(
			std::string(__func__)
			+ ": ExplicitPostTarget required for DynamicViralPostingInvoker");
	}
};

template<typename T>
using DynamicPostingPromise =
	TaggedPostingPromise<T, DynamicPostTargetThreadTag>;

template<typename T>
using DynamicViralPostingInvoker =
	ViralPostingInvoker<DynamicPostingPromise, T>;

} // namespace sscl::co

#endif // DYNAMIC_POSTING_INVOKER_H
