#ifndef AW_PROFILER_H
#define AW_PROFILER_H

// Development-only CPU profiling hook, backed by Google gperftools.
//
// This is a host-side tool: CMake links gperftools into awr_inspect on
// non-Windows hosts when it can find it, and defines AW_HAVE_PROFILER. Without
// that macro every function here compiles to a no-op, so the Windows build and
// the JNI library neither include gperftools nor carry the dependency. Nothing
// in aw_crafting includes this header; it only exists for the command line
// tools.
//
// Whether a profile is actually written is a runtime decision: pass
// --profile <file> to awr_inspect. The .prof file is read by gperftools' pprof;
// scripts/flamegraph.sh turns it into an SVG flamegraph.
//
// Sampling defaults to 100 Hz. Set the environment variable
// CPUPROFILE_FREQUENCY to a higher value (for example 1000) when a run is too
// short to gather a useful number of samples.

#include <cstdint>
#include <string>

#ifdef AW_HAVE_PROFILER
#include <gperftools/profiler.h>
#endif

namespace aw::profiler {

// True when the binary was built against gperftools.
[[nodiscard]] inline bool available() noexcept {
#ifdef AW_HAVE_PROFILER
  return true;
#else
  return false;
#endif
}

// Number of samples gathered by the current or most recent session.
[[nodiscard]] inline int64_t samples() noexcept {
#ifdef AW_HAVE_PROFILER
  ProfilerState state;
  ProfilerGetCurrentState(&state);
  return (int64_t) state.samples_gathered;
#else
  return 0;
#endif
}

// Begins sampling, writing to `path` (and truncating any previous file).
// Returns false when gperftools is not compiled in or the file cannot be
// created. Calling it while already running is a no-op that reports true.
inline bool start(const std::string& path) noexcept {
#ifdef AW_HAVE_PROFILER
  if (ProfilingIsEnabledForAllThreads())
    return true;
  return ProfilerStart(path.c_str()) != 0;
#else
  (void) path;
  return false;
#endif
}

// Stops sampling and flushes the profile to the path passed to start().
// Returns the number of samples that were gathered, or 0 when nothing ran.
inline int64_t stop() noexcept {
#ifdef AW_HAVE_PROFILER
  if (!ProfilingIsEnabledForAllThreads())
    return 0;
  const int64_t gathered = samples();
  ProfilerStop();
  return gathered;
#else
  return 0;
#endif
}

// RAII wrapper. The destructor stops the profiler, so an early return from
// main still produces a usable file.
class ScopedProfile {
 public:
  ScopedProfile() = default;
  explicit ScopedProfile(const std::string& path) { begin(path); }
  ~ScopedProfile() { end(); }

  ScopedProfile(const ScopedProfile&) = delete;
  ScopedProfile& operator=(const ScopedProfile&) = delete;
  ScopedProfile(ScopedProfile&&) = delete;
  ScopedProfile& operator=(ScopedProfile&&) = delete;

  // Starts sampling. Returns false when the profiler is unavailable, in which
  // case active() stays false and end() is a no-op.
  bool begin(const std::string& path) {
    active_ = start(path);
    return active_;
  }

  // Stops sampling and returns the sample count. Safe to call more than once.
  int64_t end() noexcept {
    if (!active_)
      return 0;
    active_ = false;
    return stop();
  }

  [[nodiscard]] bool active() const noexcept { return active_; }

 private:
  bool active_ = false;
};

}  // namespace aw::profiler

#endif
