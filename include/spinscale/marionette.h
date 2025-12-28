#ifndef _MARIONETTE_H
#define _MARIONETTE_H

#include <cstdint>
#include <atomic>
#include <memory>
#include <spinscale/component.h>

namespace sscl {

class MarionetteThread;

namespace mrntt {

class MarionetteComponent
:	public sscl::Component
{
public:
	MarionetteComponent(const std::shared_ptr<sscl::ComponentThread> &thread);
	~MarionetteComponent() = default;

public:
	typedef std::function<void(bool)> mrnttLifetimeMgmtOpCbFn;
	void initializeReq(sscl::Callback<mrnttLifetimeMgmtOpCbFn> callback);
	void finalizeReq(sscl::Callback<mrnttLifetimeMgmtOpCbFn> callback);
	// Intentionally doesn't take a callback.
	void exceptionInd();

private:
	class MrnttLifetimeMgmtOp;
	class TerminationEvent;
};

extern std::shared_ptr<sscl::MarionetteThread> thread;

extern std::atomic<int> exitCode;
void exitMarionetteLoop();
void marionetteFinalizeReqCb(bool success);
extern MarionetteComponent mrntt;

} // namespace mrntt

struct CrtCommandLineArgs
{
    CrtCommandLineArgs(int argc, char *argv[], char *envp[])
    :   argc(argc), argv(argv), envp(envp)
    {}

    int argc;
    char **argv;
    char **envp;

    static void set(int argc, char *argv[], char *envp[]);
};

} // namespace sscl

#endif // _MARIONETTE_H
