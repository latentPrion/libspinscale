#ifndef POST_TARGET_H
#define POST_TARGET_H

#include <type_traits>

#include <boost/asio/io_context.hpp>

namespace sscl::co {

/**	Opt-in dynamic post-TO target for TaggedPostingPromise coroutines.
 *	When omitted, initial_suspend posts to ThreadTag::io_context().
 *	Post-back still uses callerIoContext (getSelf() at co_await site).
 */
struct ExplicitPostTarget
{
	boost::asio::io_context& ioContext;

	explicit ExplicitPostTarget(boost::asio::io_context& ctx) noexcept
	: ioContext(ctx)
	{}
};

template<typename T>
inline constexpr bool is_explicit_post_target_v =
	std::same_as<std::remove_cvref_t<T>, ExplicitPostTarget>;

} // namespace sscl::co

#endif // POST_TARGET_H
