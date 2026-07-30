#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::e0 {

struct Snapshot final {
  std::uint64_t sequence{0};
  std::vector<double> values;
  friend bool operator==(const Snapshot &, const Snapshot &) = default;
};

struct OptionalConfig final {
  std::string name;
  std::optional<std::string> note;
};

struct Child final {
  std::string value;
};

class Parent final {
public:
  explicit Parent(std::string value) : child_{std::move(value)} {}
  [[nodiscard]] const Child &child() const noexcept { return child_; }

private:
  Child child_;
};

class SharedToken final {
public:
  explicit SharedToken(std::uint64_t value) : value_(value) { ++live_; }
  ~SharedToken() { --live_; }
  SharedToken(const SharedToken &) = delete;
  SharedToken &operator=(const SharedToken &) = delete;

  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] static std::uint64_t live() noexcept { return live_.load(); }

private:
  std::uint64_t value_;
  inline static std::atomic<std::uint64_t> live_{0};
};

inline Snapshot make_snapshot(std::size_t count) {
  Snapshot value;
  value.sequence = 7;
  value.values.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    value.values[index] = static_cast<double>(index) * 0.125;
  }
  return value;
}

inline std::uint64_t bounded_work(std::uint64_t iterations) {
  std::uint64_t value = 1469598103934665603ULL;
  for (std::uint64_t index = 0; index < iterations; ++index) {
    value ^= index + 0x9e3779b97f4a7c15ULL;
    value *= 1099511628211ULL;
  }
  return value;
}

inline void bounded_wait(std::uint64_t milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

inline std::atomic<std::uint64_t> started_recorded_waits{0};
inline std::atomic<std::uint64_t> completed_recorded_waits{0};

inline void bounded_wait_recorded(std::uint64_t milliseconds) {
  ++started_recorded_waits;
  bounded_wait(milliseconds);
  ++completed_recorded_waits;
}

inline std::uint64_t recorded_wait_start_count() noexcept {
  return started_recorded_waits.load();
}

inline std::uint64_t recorded_wait_completion_count() noexcept {
  return completed_recorded_waits.load();
}

} // namespace manyforge::e0
