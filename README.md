# libspinscale

libspinscale is a C++ coroutine and asynchronous component runtime for
thread-affine systems: applications where work is not just asynchronous, but
must run on specific long-lived component threads with explicit lifecycle,
posting, cancellation, and orchestration rules.

It is built around a simple premise: in some systems, the important question is
not only "when does this async operation complete?", but also "which component
thread owns this work, where does it resume, what locks does the coroutine
chain hold, and how does a whole subsystem start or stop as one unit?"

libspinscale targets that class of problems directly.

## What It Is For

libspinscale is meant for applications that look more like a small distributed
runtime inside one process than a collection of independent async tasks. It is a
fit when the program has:

- Dedicated component threads with their own `boost::asio::io_context`.
- Thread-affine components that must run operations on their owning thread.
- Startup, pause, resume, and shutdown protocols across many threads.
- Coroutine orchestration that needs structured fan-out and settlement scanning.
- Cross-thread post-to and post-back behavior that must be explicit and
  predictable.
- Coroutine-aware locking where deadlock detection must understand the caller
  coroutine chain.

Typical examples include simulation runtimes, test harnesses, embedded control
planes, game/server subsystems, robotics-style component graphs, multi-thread
workflow engines, and other applications where "run this on the right thread" is
part of the correctness contract.

## What Makes It Different

Most C++ coroutine libraries focus on awaitable primitives, task types, event
loops, generators, or composable async algorithms. libspinscale focuses on
thread-affine component orchestration. We'll compare libspinscale to contemporaneous C++ coro libraries below to differentiate it:

Boost.Asio gives excellent networking and executor primitives, but it does not
define an application-level component model. libspinscale uses Asio
`io_context`s as posting threads and layers a stricter coroutine model on top:
component threads, post-to/post-back promises, lifecycle invokers, and
structured group settlement.

Folly coroutines provide production-grade task abstractions and executor
integration, especially for large service stacks. libspinscale is narrower: it
optimizes for explicit component ownership and deterministic thread handoff
rather than general-purpose async service composition.

cppcoro provides foundational coroutine types such as tasks, generators, async
manual reset events, and related primitives. libspinscale is less of a primitive
toolkit and more of a runtime pattern for systems with named threads, component
lifetimes, and cross-thread orchestration.

In short: libspinscale is not trying to replace Boost.Asio, Folly, or cppcoro.
It is trying to solve the coordination problem that appears above them when an
application has a fixed topology of cooperating component threads.

## Core Ideas

### Component Threads

`ComponentThread` owns a `boost::asio::io_context` and represents a named
execution thread. `PuppeteerThread` and `PuppetThread` build a lifecycle model
on top of that thread:

- A puppeteer coordinates the application.
- Puppet threads host component work.
- Lifecycle operations such as JOLT, start, pause, resume, and exit are exposed
  as awaitable operations.

`PuppetApplication` orchestrates a set of puppet threads and exposes lifecycle
batch operations:

```cpp
co_await app.joltAllPuppetThreadsCReq();
co_await app.startAllPuppetThreadsCReq();
co_await app.exitAllPuppetThreadsCReq();
```

These operations are coroutine-native. Callers that are already in a coroutine
can `co_await` them directly.

### Posting Promises

Posting coroutines are tied to a target thread. A tagged posting promise posts the
callee coroutine to `ThreadTag::io_context()` at initial suspend and posts back
to the caller's `io_context` at completion.

```cpp
struct BodyThreadTag
{
	static boost::asio::io_context &io_context();
};

template<typename T>
using BodyPostingPromise =
	sscl::co::TaggedPostingPromise<T, BodyThreadTag>;

template<typename T>
using BodyInvoker =
	sscl::co::ViralPostingInvoker<BodyPostingPromise, T>;
```

This makes thread ownership visible in the coroutine return type. A component
operation can say, at the type level, that it runs on the body thread and resumes
its caller correctly when done.

### Viral And Non-Viral Invokers

libspinscale distinguishes coroutine-to-coroutine orchestration from
non-coroutine entry points.

Viral invokers are awaitable and are used inside coroutine call chains:

```cpp
BodyInvoker<void> BodyComponent::initializeCReq()
{
	co_return;
}

co_await body.initializeCReq();
```

Non-viral invokers are for top-level boundaries where ordinary code starts a
coroutine and supplies a completion callback:

```cpp
auto invoker = component.initializeFromHookCReq(exceptionPtr, [] {
	// completion callback
});
```

This distinction is intentional. Coroutine orchestration should use `co_await`.
Callback-style completion should stay at the outer boundary.

### Dynamic Post Targets

Most posting coroutines use a compile-time `ThreadTag`. When the target thread is
known only at runtime, `DynamicViralPostingInvoker<T>` and
`ExplicitPostTarget` allow the caller to supply the destination `io_context`:

```cpp
sscl::co::DynamicViralPostingInvoker<void>
runOnSelectedThread(sscl::co::ExplicitPostTarget target)
{
	co_return;
}

co_await runOnSelectedThread(sscl::co::ExplicitPostTarget{thread.getIoContext()});
```

The post-back side still returns to the caller's thread.

### Group Settlement

`co::Group` provides structured fan-out over heterogeneous invokers. Members can
be added, awaited as a group, and inspected by settlement status:

```cpp
sscl::co::Group group;

auto bodyInit = body.initializeCReq();
auto worldInit = world.initializeCReq();
auto legInit = leg.initializeCReq();

group.add(bodyInit);
group.add(worldInit);
group.add(legInit);

co_await group.getAwaitAllSettlementsInvoker();
group.checkForAndReThrowGroupExceptions();
```

Settlements record whether a member completed or threw. The original invoker can
be recovered from a descriptor when a caller needs typed return values.

### Non-Viral Task Nursery

`co::NonViralTaskNursery` is the structured-concurrency owner for non-viral
invokers at non-coroutine boundaries. Unlike `co::Group`, it is for callback-style
entry from ordinary code (HTTP handlers, timers, shutdown sequences), not for
`co_await` orchestration inside coroutines.

```cpp
sscl::co::NonViralTaskNursery nursery;
nursery.openAdmission();

auto lease = nursery.getNewSlotLease();
lease.getSyncCanceler().startAcceptingWork();
lease.setOnSettledHook(
	[](std::exception_ptr &exceptionPtr)
	{
		sscl::co::NonViralCompletion nvc(exceptionPtr);
		nvc.checkAndRethrowException();
	});
lease.fillSlot(
	[&lease]()
	{
		return component.someNonViralCReq(
			lease.getExceptionStorage(),
			lease.getCallerLambda(),
			lease.getSyncCanceler());
	});
lease.commit();

nursery.requestCancelOnAll();
nursery.closeAdmission();
nursery.syncAwaitAllSettlements(ioContext);
```

Each slot owns a `SyncCancelerForAsyncWork`. `requestCancelOnAll()` only signals
cooperative stop; it does not destroy invokers. Invokers are retired when their
completion callbacks run. Call `closeAdmission()` explicitly before
`asyncAwaitAllSettlements()` or `syncAwaitAllSettlements()`; those APIs wait until
all slots have retired naturally and throw if admission is still open.

`Slot::Lease` is commit-required: an uncommitted lease removes its reservation on
destruction. `fillSlot()` takes an invoker factory (deferred construction) because
non-viral coroutines may complete synchronously during invoker construction.
The factory may capture `lease` by reference; `setOnSettledHook()` may not
capture `lease` itself. The nursery passes the slot's `exceptionPtr` into the
hook at retirement.
`Slot::Handle` is an opaque slot pointer valid only while the slot remains in the
nursery.

Slot metadata (`exceptionPtr`, lease/settlement status, canceler) lives on `Slot`.
`MemberInvokerBase` is invoker type-erasure only.

### Coroutine-Aware Locking

`co::CoQutex` is a coroutine-aware mutual exclusion primitive. It tracks
acquired locks through the coroutine promise chain, which lets it detect
dangerous re-acquisition patterns across nested coroutine calls instead of only
within one stack frame.

```cpp
auto releaseHandle = co_await qutex.getAcquireInvocationAndSuspensionPolicy();
```

When a coroutine cannot acquire the qutex, it suspends and is resumed through
the correct caller `io_context`.

## Design Biases

libspinscale intentionally favors:

- Explicit thread ownership over invisible executor selection.
- Member coroutine APIs over free-function workaround layers.
- `co_await` orchestration inside coroutine code.
- Callback completion only at non-coroutine boundaries.
- Structured group settlement over ad hoc counters and flags.
- Type-visible posting behavior over ambient global schedulers.

These choices make the library opinionated. That is the point. It is designed
for systems where implicit scheduling is a source of bugs.

## Non-Goals

libspinscale is not a general replacement for:

- Boost.Asio networking and executor facilities.
- Folly's broad async service infrastructure.
- cppcoro's foundational coroutine primitive set.
- Standard library coroutine machinery.

It also is not trying to hide C++ coroutine mechanics. Promise types, invokers,
and suspension behavior are part of the public design because the target
applications need control over where work runs and where completion resumes.

## Status

The API is still evolving. The current direction is centered on coroutine-native
component orchestration:

- `boost::asio::io_context` is the thread event-loop primitive.
- Posting coroutines use `TaggedPostingPromise<T, ThreadTag>`.
- Runtime-selected posting uses `DynamicViralPostingInvoker<T>`.
- Component lifecycle batches are viral non-posting coroutines.
- `co::Group` is the primary structured fan-out/fan-in primitive.
- `co::NonViralTaskNursery` owns non-viral invoker lifetimes at outer boundaries.

Expect breaking changes when they simplify the ownership, lifecycle, or
post-to/post-back model.
