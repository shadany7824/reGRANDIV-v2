/**
 * @file        core/clock_posix.cpp
 * @brief       POSIX platform clock implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#if defined(__EMSCRIPTEN__)
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <emscripten/emscripten.h>
#endif

#if REX_PLATFORM_PS5
// [ps5 port] clock_gettime is a system call on the PS5; the guest reads the
// clock (mftb, KeQueryPerformanceCounter) tens of thousands of times a second.
// The TSC is invariant and in step across the cores, as titles rely on.
extern "C" uint64_t sceKernelReadTsc(void);
extern "C" uint64_t sceKernelGetTscFrequency(void);
namespace {
uint64_t Ps5TscFrequency() {
  static const uint64_t frequency = sceKernelGetTscFrequency();
  return frequency;
}
}  // namespace
#endif

namespace rex::chrono {

uint64_t Clock::host_tick_frequency_platform() {
#if REX_PLATFORM_PS5
  if (const uint64_t frequency = Ps5TscFrequency()) return frequency;
#endif
#ifdef __APPLE__
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  return (uint64_t)((1000000000ull * (uint64_t)info.denom) / (uint64_t)info.numer);
#else
  // host_tick_count_platform() counts nanoseconds, so this is 1 GHz whatever
  // the clock's resolution. (It used to be 1e9 / clock_getres(): 1 ns on Linux,
  // but FreeBSD reports 1e9 / timecounter frequency + 1, more than 1 ns on a
  // PS5, and the guest clock ran that many times too fast.)
  return 1000000000ull;
#endif
}

uint64_t Clock::host_tick_count_platform() {
#ifdef __APPLE__
  // mach_absolute_time() returns raw hardware ticks; host_tick_frequency_platform()
  // expresses the rate of these same ticks via mach_timebase_info.
  // clock_gettime(CLOCK_MONOTONIC_RAW) returns nanoseconds, which would be
  // inconsistent with the 24 MHz mach frequency and cause ~41x time acceleration.
  return mach_absolute_time();
#else
#if REX_PLATFORM_PS5
  if (Ps5TscFrequency()) return sceKernelReadTsc();
#endif
#if defined(__EMSCRIPTEN__)
  // [web port] Each read is a call out to JavaScript (performance.now), and
  // the game reads the clock (mftb, KeQueryPerformanceCounter) hundreds of
  // thousands of times a second: a tenth of its main thread went to it. So a
  // thread reading it in a tight loop - 128 times or more in the last
  // millisecond (WebClockEpoch, bumped by a ticker thread) - gets its last
  // reading plus a nanosecond, up to 15 times, then the real time again: a
  // few microseconds behind at most, and always going forward. Other threads
  // always read the real time: one reading it now and then would otherwise
  // get a reading up to a millisecond old, and the game's frame pacing,
  // waiting on such readings, made every frame a millisecond long (55 frames
  // a second, not 60). Before the ticker runs (StartWebClockTicker, at
  // startup) every read is a real one, and always with REX_WEB_EXACT_CLOCK
  // set (?env=REX_WEB_EXACT_CLOCK=1).
  thread_local uint64_t last = 0;
  thread_local uint32_t reads = 0, epoch = 0, in_epoch = 0;
  thread_local bool hot = false;
  const uint32_t now_epoch = WebClockEpoch().load(std::memory_order_relaxed);
  if (now_epoch != epoch) {
    hot = in_epoch >= 128;
    in_epoch = 0;
    epoch = now_epoch;
    reads = 16;  // (a real read first)
  }
  in_epoch++;
  if (now_epoch && hot && ++reads < 16) return ++last;
  reads = 0;
  const uint64_t ns = uint64_t(emscripten_get_now() * 1e6);
  last = ns > last ? ns : last + 1;
  return last;
#else
  timespec tp;
  int error = clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
  assert_zero(error);

  return tp.tv_nsec + tp.tv_sec * 1000000000ull;
#endif
#endif
}

#if defined(__EMSCRIPTEN__)
std::atomic<uint32_t>& WebClockEpoch() {
  static std::atomic<uint32_t> epoch{0};
  return epoch;
}

void StartWebClockTicker() {
  if (std::getenv("REX_WEB_EXACT_CLOCK")) return;
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      for (;;) {
        const timespec millisecond{0, 1000000};
        nanosleep(&millisecond, nullptr);
        WebClockEpoch().fetch_add(1, std::memory_order_relaxed);
      }
    }).detach();
  });
}
#endif

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
