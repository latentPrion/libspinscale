#ifndef BOOST_ASIO_LINKAGE_FIX_H
#define BOOST_ASIO_LINKAGE_FIX_H

#include <boost/asio/detail/call_stack.hpp>
#include <boost/asio/detail/thread_context.hpp>
#include <boost/asio/detail/tss_ptr.hpp>

namespace boost {
namespace asio {
namespace detail {

/**	EXPLANATION:
 * Extern declaration of the template instantiation
 * This ensures that the .o translation units don't have their
 * own copies of `call_stack<>::top_` defined in them.
 */
extern template
tss_ptr<call_stack<thread_context, thread_info_base>::context>
call_stack<thread_context, thread_info_base>::top_;

} // namespace detail
} // namespace asio
} // namespace boost

#endif // BOOST_ASIO_LINKAGE_FIX_H
