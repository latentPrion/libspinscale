#include <boost/asio/detail/call_stack.hpp>
#include <boost/asio/detail/thread_context.hpp>
#include <boost/asio/detail/tss_ptr.hpp>

namespace boost {
namespace asio {
namespace detail {

/** Single translation-unit definition for Boost.Asio call_stack TLS.
 * Other TUs include boostAsioLinkageFix.h first and use extern template.
 */
template
tss_ptr<call_stack<thread_context, thread_info_base>::context>
call_stack<thread_context, thread_info_base>::top_;

} // namespace detail
} // namespace asio
} // namespace boost
