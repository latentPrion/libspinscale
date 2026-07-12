#ifndef SPINSCALE_TEST_SUPPORT_PROBE_COMPONENT_THREAD_H
#define SPINSCALE_TEST_SUPPORT_PROBE_COMPONENT_THREAD_H

/**	EXPLANATION:
 * Compatibility shim: probe harness lives in spinscale_probe_support under
 * sscl::probe. Test code may keep including this path and using sscl::tests
 * names; tools should include <probe/probeComponentThread.h> directly.
 */

#include <probe/probeComponentThread.h>

namespace sscl::tests {

using sscl::probe::defaultProbeTaskTimeout;
using sscl::probe::runNonViralNurseryOnComponentThread;
using sscl::probe::ProbeComponentThreadHarness;

} // namespace sscl::tests

#endif // SPINSCALE_TEST_SUPPORT_PROBE_COMPONENT_THREAD_H
