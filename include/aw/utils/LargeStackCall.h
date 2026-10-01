#ifndef LARGE_STACK_CALL_H
#define LARGE_STACK_CALL_H

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <type_traits>
#include <utility>

// Run a callable on a purpose-built stack, so that a recursion that does not fit
// in the calling thread's stack still runs to completion. The planner is entered
// from a JVM thread, whose stack is sized for the mod's Java code and not for a
// traversal over every recipe in the game.
//
// The call stays ordinary: `call` returns once the callable has, a nested call
// works, and the callable must not let anything propagate out of it, since the
// library is built without exceptions. Only the stack the callable runs on is
// different, and only for as long as it runs.
//
//   * Windows x64: a fiber, which is the platform's own way of running work on a
//     stack the thread does not normally use. 32-bit Windows is not supported by
//     the mod and falls back to the calling thread's stack.
//   * x86-64 System V: the stack is malloc'd, rsp is switched to it and switched
//     back afterwards.
//   * Anything else, aarch64 included: the callable runs on the calling thread's
//     stack.
#if defined(_WIN64)

// The fiber backend needs the fiber API. windows.h must be included here, at
// global scope and before anything else: inside a namespace its declarations
// would land in that namespace and the C library headers included later would
// not find them. Its min and max macros would break std::min and std::max in
// every translation unit that includes this header, so they are turned off here
// and stay off for the rest of such a unit.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define AW_LS_BACKEND 1

#elif defined(__x86_64__)

#  define AW_LS_BACKEND 2

#else

#  define AW_LS_BACKEND 0

#endif

namespace aw::ls {

namespace detail {

// The callable and the arguments it is entered with. Both live on the calling
// thread's stack, which stays untouched underneath the new one, so nothing here
// has to be copied into the new stack.
template <typename Func, typename... Args>
struct Job {
  Func func;
  std::tuple<Args...> args;
};

template <typename Func, typename... Args>
void runJob(void *raw) {
  Job<Func, Args...> &job = *static_cast<Job<Func, Args...> *>(raw);
  std::apply(job.func, job.args);
}

// A job could not be given its own stack. The call is still correct on the
// calling thread's stack, only bounded by it, so this is a warning rather than a
// hard failure: a deep recursion may then take the host process down instead of
// finishing, which is exactly the trade the backends below make.
inline void warnNoStack(const char *reason) {
  std::fprintf(stderr, "aw::ls::call: cannot %s; using the thread stack\n", reason);
}

#if AW_LS_BACKEND == 1

// Runs the job on a fiber with a stack of its own, then switches back to the
// fiber the calling thread was already running on.
struct FiberJob {
  void (*fn)(void *);
  void *ctx;
  void *returnTo;
};

// Entry point of the worker fiber. The switch back is explicit because Windows
// does not document what happens when a fiber procedure returns, and the caller
// has to be resumed before the fiber is deleted either way.
inline void WINAPI fiberEntry(void *raw) {
  const FiberJob &job = *static_cast<const FiberJob *>(raw);
  job.fn(job.ctx);
  SwitchToFiber(job.returnTo);
}

// The fiber the calling thread is running on right now, which is where the job
// has to switch back to. A thread must be converted with ConvertThreadToFiber
// before it can switch to another fiber. The conversion is deliberately not
// undone: the mod owns one long-lived worker thread and does not know when its
// last plan has run, and a nested call finds the thread already converted and
// switches back to the fiber it is running on rather than to the thread's. That
// is what keeps the switch back local to the call that made it.
//
// Returns null only if the thread could not be converted at all.
inline void *currentFiber() {
  static thread_local bool converted = false;
  if (!converted) {
    if (ConvertThreadToFiber(nullptr) == nullptr && GetLastError() != ERROR_ALREADY_FIBER)
      return nullptr;
    converted = true;
  }
  return GetCurrentFiber();
}

inline void runOnFiber(std::size_t stackSize, void (*fn)(void *), void *ctx) {
  void *returnTo = currentFiber();
  FiberJob job{fn, ctx, returnTo};

  // A zero commit size keeps the fiber's initial commit at the executable's
  // default and lets it grow into the reserve on demand, one guard page at a
  // time, so a large reserve costs address space rather than memory. Zero flags
  // because FIBER_FLAG_FLOAT_SWITCH only matters on x86 (and on the unsupported
  // 32-bit x86 at that); x64 preserves the floating point state always.
  void *worker =
      returnTo != nullptr ? CreateFiberEx(0, stackSize, 0, &fiberEntry, &job) : nullptr;
  if (worker == nullptr) {
    warnNoStack("create a fiber");
    fn(ctx);
    return;
  }

  SwitchToFiber(worker);
  DeleteFiber(worker);
}

#elif AW_LS_BACKEND == 2

// Enter `fn(ctx)` with rsp just below `stackTop`, then return.
//
// This has to be a real function rather than inline asm: arbitrary code running
// on the new stack may clobber every caller-saved register, and only an actual
// call makes the compiler spill whatever it happens to keep in them. An inline
// asm block would have to list the clobber set of an unknown callee, which it
// cannot.
extern "C" void awSwitchStack(void *stackTop, void (*fn)(void *), void *ctx);

// Defined in the header, weakly, so that including it from several translation
// units does not yield duplicate symbols: the linker keeps one copy. Hidden, so
// that the call is bound inside the library and needs no PLT entry, and the
// symbol is not exported from the JNI library.
asm(R"(
.text
.weak awSwitchStack
.hidden awSwitchStack
.type awSwitchStack, @function
awSwitchStack:
	.cfi_startproc
	# Save the caller's frame pointer. The old rsp is kept in rbp, which the
	# callee preserves, so it survives the call. There is no red zone to worry
	# about here: this is a normal function call.
	pushq %rbp
	.cfi_def_cfa_offset 16
	.cfi_offset %rbp, -16
	movq %rsp, %rbp
	.cfi_def_cfa_register %rbp
	# Switch to the new stack, 16-byte aligned as the ABI requires. `callq`
	# then pushes the return address, leaving rsp == 8 (mod 16) on entry.
	movq %rdi, %rsp
	andq $-16, %rsp
	movq %rdx, %rdi
	callq *%rsi
	# Back on the caller's stack.
	movq %rbp, %rsp
	.cfi_def_cfa %rsp, 8
	popq %rbp
	ret
	.cfi_endproc
	.size awSwitchStack, .-awSwitchStack
)");

#endif

// Hand the job to the platform's stack, for whichever backend is selected.
template <std::size_t StackSize>
void dispatch(void (*fn)(void *), void *ctx) {
#if AW_LS_BACKEND == 1

  runOnFiber(StackSize, fn, ctx);

#elif AW_LS_BACKEND == 2

  static char *stack = static_cast<char *>(std::malloc(StackSize));
  if (stack == nullptr) {
    warnNoStack("allocate the stack");
    fn(ctx);
    return;
  }

  awSwitchStack(stack + StackSize, fn, ctx);

#else

  fn(ctx);

#endif
}

}

// Generous: a frame of the traversals below is a few hundred bytes, so this
// covers graphs much larger than anything the mod can produce. On Linux it is
// mmap backed, and on Windows it is only reserved, so in both cases the pages
// that are never touched are never committed.
inline constexpr size_t defaultStackSize = 64 * 1024 * 1024;

template <size_t StackSize = defaultStackSize, typename Func, typename... Args>
void call(Func &&func, Args &&...args) {
  // The arguments are forwarded rather than decayed: `Args` keeps the reference
  // for an lvalue argument, so a reference parameter such as the crafting graph
  // is not copied, while a temporary is stored by value inside the job.
  using Job = detail::Job<std::decay_t<Func>, Args...>;
  Job job{std::forward<Func>(func), std::tuple<Args...>(std::forward<Args>(args)...)};

  detail::dispatch<StackSize>(&detail::runJob<std::decay_t<Func>, Args...>, &job);
}

}

#undef AW_LS_BACKEND

#endif
