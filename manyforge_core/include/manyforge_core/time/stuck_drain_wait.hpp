#pragma once

#include <chrono>
#include <cstdio>
#include <string_view>

namespace manyforge::core {

/// Typed code of the line written when a drain misses its bound.
inline constexpr std::string_view kDrainStuckCode = "drain_stuck";

/// The first missed drain bound in this process, or nullptr. The returned name
/// has static storage and remains valid for the process lifetime.
[[nodiscard]] const char* FirstStuckDrainName() noexcept;

namespace detail {
/// `drain` must be a non-null, static-storage name (normally a literal).
void RecordStuckDrain(const char* drain) noexcept;
}  // namespace detail

/// Waits for a drain that must complete before its owner may continue.
///
/// `drain_for(bound)` returns true once drained. A drain that misses the
/// bound is not a crash: one typed line names it, `on_stuck` runs once (e.g.
/// a flight-recorder dump), and the wait continues until the drain completes
/// or the operator ends the process. A second line records a late drain.
/// An exception from `drain_for` propagates unchanged to the caller.
/// `drain` must be a non-null, static-storage name (normally a literal).
template <typename DrainFor, typename OnStuck>
void WaitForDrain(const char* drain, std::chrono::nanoseconds bound, DrainFor&& drain_for,
                  OnStuck&& on_stuck) {
  if (drain_for(bound)) return;
  detail::RecordStuckDrain(drain);
  const auto bound_ms = std::chrono::duration_cast<std::chrono::milliseconds>(bound).count();
  (void)std::fprintf(stderr,
                     "manyforge_fault code=%.*s drain=%s bound_ms=%lld "
                     "state=waiting_for_operator\n",
                     static_cast<int>(kDrainStuckCode.size()), kDrainStuckCode.data(),
                     drain, static_cast<long long>(bound_ms));
  try {
    on_stuck();
  } catch (...) {
  }
  while (!drain_for(bound)) {
  }
  (void)std::fprintf(stderr, "manyforge_fault code=%.*s drain=%s state=drained_late\n",
                     static_cast<int>(kDrainStuckCode.size()), kDrainStuckCode.data(),
                     drain);
}

template <typename DrainFor>
void WaitForDrain(const char* drain, std::chrono::nanoseconds bound,
                  DrainFor&& drain_for) {
  WaitForDrain(drain, bound, static_cast<DrainFor&&>(drain_for), [] {});
}

}  // namespace manyforge::core
