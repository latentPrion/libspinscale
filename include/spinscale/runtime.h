#ifndef RUNTIME_H
#define RUNTIME_H

namespace sscl {

struct CrtCommandLineArgs
{
	CrtCommandLineArgs(int argc, char *argv[], char *envp[])
	:	argc(argc), argv(argv), envp(envp)
	{}

	int argc;
	char **argv;
	char **envp;

	static void set(int argc, char *argv[], char *envp[]);
};

extern CrtCommandLineArgs crtCommandLineArgs;

} // namespace sscl

#endif // RUNTIME_H
