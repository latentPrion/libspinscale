#include <spinscale/runtime.h>

namespace sscl {

CrtCommandLineArgs crtCommandLineArgs(0, nullptr, nullptr);

void CrtCommandLineArgs::set(int argc, char *argv[], char *envp[])
{
	crtCommandLineArgs = CrtCommandLineArgs(argc, argv, envp);
}

} // namespace sscl
