#ifndef SPINSCALE_CALLABLE_TRACER_H
#define SPINSCALE_CALLABLE_TRACER_H

#include <config.h>
#include <string>
#include <functional>
#include <iostream>
#include <cstdint>
#include <spinscale/componentThread.h>

// Forward declaration - OptionParser is defined in smocore/include/opts.h
// If you need tracing, include opts.h before including this header
// The code will check for OPTS_H define to see if opts.h has been included
class OptionParser;

namespace sscl {

/**
 * @brief CallableTracer - Wraps callables with metadata for debugging
 *
 * This class wraps any callable object with metadata (caller function name,
 * line number, and return addresses) to help debug cases where callables
 * posted to boost::asio::io_service have gone out of scope. The metadata
 * can be accessed from the callable's address when debugging.
 */
class CallableTracer
{
public:
	/**
	 * @brief Constructor that wraps a callable with metadata
	 * @param callerFuncName The name of the function that created this callable
	 * @param callerLine The line number where this callable was created
	 * @param returnAddr0 The return address of the direct caller
	 * @param returnAddr1 The return address of the caller before that
	 * @param callable The callable object to wrap
	 */
	template<typename CallableT>
	explicit CallableTracer(
		const char* callerFuncName,
		int callerLine,
		void* returnAddr0,
		void* returnAddr1,
		CallableT&& callable)
	: callerFuncName(callerFuncName),
	  callerLine(callerLine),
	  returnAddr0(returnAddr0),
	  returnAddr1(returnAddr1),
	  callable(std::forward<CallableT>(callable))
	{}

	void operator()()
	{
		// OptionParser::getOptions() requires opts.h to be included
		// Only check traceCallables if opts.h has been included (OPTS_H is defined)
		#ifdef CONFIG_DEBUG_TRACE_CALLABLES
		#ifdef OPTS_H
		if (OptionParser::getOptions().traceCallables)
		{
			std::cout << "" << __func__ << ": On thread "
				<< (ComponentThread::tlsInitialized()
					? ComponentThread::getSelf()->name : "<TLS un-init'ed>")
					<< ": Calling callable posted by:\n"
				<< "\t" << callerFuncName << "\n\tat line " << (int)callerLine
				<< " return addr 0: " << returnAddr0
				<< ", return addr 1: " << returnAddr1
				<< std::endl;
		}
		#endif
		#endif
		callable();
	}

public:
	/// Name of the function that created this callable
	std::string callerFuncName;
	/// Line number where this callable was created
	int callerLine;
	/// Return address of the direct caller
	void* returnAddr0;
	/// Return address of the caller before that
	void* returnAddr1;

private:
	/// The wrapped callable (type-erased using std::function)
	std::function<void()> callable;
};

} // namespace sscl

/**
 * @brief STC - SMO Traceable Callable macro
 *
 * When CONFIG_DEBUG_TRACE_CALLABLES is defined, wraps the callable with
 * CallableTracer to store metadata (caller function name, line number,
 * and return addresses). When not defined, returns the callable directly
 * with no overhead.
 *
 * Uses compiler-specific macros to get fully qualified function names:
 * - GCC/Clang: __PRETTY_FUNCTION__ (includes full signature with namespace/class)
 * - MSVC: __FUNCSIG__ (includes full signature)
 * - Fallback: __func__ (unqualified function name only)
 *
 * Uses compiler-specific builtins to get return addresses:
 * - GCC/Clang: __builtin_return_address(0) and __builtin_return_address(1)
 * - MSVC: _ReturnAddress() (only one level available)
 * - Fallback: nullptr for return addresses
 *
 * Usage:
 *   thread->getIoService().post(
 *       STC(std::bind(&SomeClass::method, this, arg1, arg2)));
 */
#ifdef CONFIG_DEBUG_TRACE_CALLABLES
	#if defined(__GNUC__) || defined(__clang__)
		// GCC/Clang: __PRETTY_FUNCTION__ gives full signature
		// e.g., "void smo::SomeClass::method(int, int)"
		// __builtin_return_address(0) = direct caller
		// __builtin_return_address(1) = caller before that
		#define STC(arg) smo::CallableTracer( \
			__PRETTY_FUNCTION__, \
			__LINE__, \
			__builtin_return_address(0), \
			__builtin_return_address(1), \
			arg)
	#elif defined(_MSC_VER)
		// MSVC: __FUNCSIG__ gives full signature
		// e.g., "void __cdecl smo::SomeClass::method(int, int)"
		// _ReturnAddress() = direct caller (only one level available)
		#include <intrin.h>
		#define STC(arg) smo::CallableTracer( \
			__FUNCSIG__, \
			__LINE__, \
			_ReturnAddress(), \
			nullptr, \
			arg)
	#else
		// Fallback to standard __func__ (unqualified name only)
		// No return address support
		#define STC(arg) smo::CallableTracer( \
			__func__, \
			__LINE__, \
			nullptr, \
			nullptr, \
			arg)
	#endif
#else
#define STC(arg) arg
#endif

#endif // SPINSCALE_CALLABLE_TRACER_H
