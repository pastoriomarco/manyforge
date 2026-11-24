#pragma once

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// CP9 component stream health (plan `component-liveness-and-interface-2026-10-26`).
/// Adapters declare streams and push samples; component states read the
/// classified freshness.

enum class StreamSeverityDTO : std::uint8_t {
  /// Lost component: motion blocked; after the target was reached a
  /// `provider_lost` incident (Full Stop, alarm, Recover).
  kFault = 0,
  /// Shown only.
  kWarning,
};

enum class StreamTimestampSourceDTO : std::uint8_t {
  /// A sample advances only when its source stamp changes (an equal stamp = paused source).
  kMessageStamp = 0,
  /// Every sample advances (no usable stamp).
  kArrival,
};

enum class StreamLivenessStateDTO : std::uint8_t { kLive = 0, kStale, kAbsent };

/// Typed condition codes (`StreamHealthDTO::error.code`, Kernel refusal
/// `details.reason`, incident diagnostic code).
inline constexpr std::string_view kComponentStreamStaleCode = "component_stream_stale";
inline constexpr std::string_view kComponentStreamAbsentCode = "component_stream_absent";

inline constexpr std::size_t kMaxStreamIdBytes = 64U;

/// 1..64 bytes of `[a-z0-9_./-]`.
[[nodiscard]] bool ValidStreamId(std::string_view value) noexcept;

/// One declared stream with its adapter-kind defaults.
struct StreamDeclarationDTO final {
  std::string stream_id;
  std::int64_t budget_ns{};
  StreamSeverityDTO severity{StreamSeverityDTO::kFault};
  StreamTimestampSourceDTO timestamp_source{StreamTimestampSourceDTO::kMessageStamp};

  [[nodiscard]] bool locally_valid() const noexcept {
    return ValidStreamId(stream_id) && budget_ns > 0;
  }
  friend bool operator==(const StreamDeclarationDTO&, const StreamDeclarationDTO&) = default;
};

/// One Deployment override (`policies.component_streams` row).
struct StreamParameterOverrideDTO final {
  ControllableComponentId controllable_component_id;
  std::string stream_id;
  std::optional<std::int64_t> budget_ns;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !controllable_component_id.empty() && ValidStreamId(stream_id) &&
           budget_ns && *budget_ns > 0;
  }
  friend bool operator==(const StreamParameterOverrideDTO&,
                         const StreamParameterOverrideDTO&) = default;
};

/// Reported health of one stream (effective parameters).
struct StreamHealthDTO final {
  std::string stream_id;
  StreamLivenessStateDTO state{StreamLivenessStateDTO::kAbsent};
  /// Host-clock time since the last advancing sample; absent when none.
  std::optional<std::int64_t> age_ns;
  std::int64_t budget_ns{};
  StreamSeverityDTO severity{StreamSeverityDTO::kFault};
  StreamTimestampSourceDTO timestamp_source{StreamTimestampSourceDTO::kMessageStamp};
  /// Active condition (stale/absent).
  std::optional<ErrorDTO> error;

  friend bool operator==(const StreamHealthDTO&, const StreamHealthDTO&) = default;
};

/// The classified conditions of one component. Errors carry no age, so a
/// verdict changes only on a transition.
struct ComponentStreamVerdictDTO final {
  /// A `fault` stream is stale or absent: incident diagnostic once reached.
  std::optional<ErrorDTO> fault_loss;

  [[nodiscard]] bool empty() const noexcept { return !fault_loss; }
  friend bool operator==(const ComponentStreamVerdictDTO&,
                         const ComponentStreamVerdictDTO&) = default;
};

class ComponentStreamHealth;

/// Wait-free view of one component's stream-stale condition, kept by the
/// registry at every classification: true while a `fault` stream of the
/// component is stale, which is the condition that faults the component as
/// stream-stale (effective budget with the Deployment overrides, message
/// stamps). One atomic read, so time-critical paths may read it.
struct ComponentStreamStaleFlag final {
  [[nodiscard]] bool stale() const noexcept { return value.load(std::memory_order_acquire); }

  std::atomic<bool> value{false};
};

/// Adapter handle of one declared stream. Destruction undeclares it.
/// `Record*` are lock-free and allocation-free unless the stream was not
/// live (one transition notification). The registry's clock must outlive it.
class StreamRecorder final {
 public:
  ~StreamRecorder();
  StreamRecorder(const StreamRecorder&) = delete;
  StreamRecorder& operator=(const StreamRecorder&) = delete;

  /// One sample with its source stamp (ns; any change advances, e.g. a sim time reset).
  /// For `arrival` streams the stamp is ignored.
  void Record(std::int64_t source_stamp_ns) noexcept;
  /// One sample without a usable stamp.
  void RecordArrival() noexcept;

  [[nodiscard]] const ControllableComponentId& component() const noexcept;
  [[nodiscard]] const std::string& stream_id() const noexcept;

  struct Slot;

 private:
  friend class ComponentStreamHealth;
  explicit StreamRecorder(std::shared_ptr<Slot> slot) noexcept;
  std::shared_ptr<Slot> slot_;
};

/// Registry of every declared stream of one process.
///
/// One deadline timer for the earliest deadline (a heap of lazily refreshed
/// deadlines; no polling, no scan of all streams). All classification and
/// every log call run on the scheduler's Monitoring lane; mutating
/// calls only mark the component and schedule that evaluation.
class ComponentStreamHealth final {
 public:
  ComponentStreamHealth(std::shared_ptr<IClock> clock, IScheduler& scheduler);
  ~ComponentStreamHealth();
  /// Retire callbacks before the borrowed scheduler and clock are destroyed.
  void DetachScheduler() noexcept;
  [[nodiscard]] bool DetachSchedulerUntil(std::chrono::steady_clock::time_point deadline) noexcept;
  ComponentStreamHealth(const ComponentStreamHealth&) = delete;
  ComponentStreamHealth& operator=(const ComponentStreamHealth&) = delete;

  /// Declares one stream with its adapter-kind defaults; a Deployment
  /// override applies on top. Replaces an earlier declaration of the same
  /// `(component, stream_id)`. The stream starts `absent`, classified after
  /// one budget.
  [[nodiscard]] Result<std::unique_ptr<StreamRecorder>> Declare(
      const ControllableComponentId& component, StreamDeclarationDTO defaults);

  /// Host bridge for adapter processes (Python sink): records into the
  /// stream the host-side adapter declared. False when it is not declared.
  /// `nullopt` stamp = arrival only.
  bool RecordDeclared(const ControllableComponentId& component, std::string_view stream_id,
                      std::optional<std::int64_t> source_stamp_ns);

  /// Replaces every Deployment override (product install). Invalid rows are
  /// refused as a whole and nothing changes.
  [[nodiscard]] Result<void> SetOverrides(std::vector<StreamParameterOverrideDTO> overrides);

  /// Current health of every stream of the component, sorted by stream id.
  [[nodiscard]] std::vector<StreamHealthDTO> Read(const ControllableComponentId& component) const;
  /// The component's stream-stale flag. It follows every later classification
  /// of the component, including streams declared or replaced afterwards.
  [[nodiscard]] std::shared_ptr<const ComponentStreamStaleFlag> Watch(
      const ControllableComponentId& component);
  /// Optional edge-triggered transition log (one line per verdict change).
  void SetLog(std::function<void(const std::string&)> log);

  /// Classifies due deadlines and pending transitions now (the scheduled
  /// callback; also usable inline by tests).
  void Evaluate();

  struct Shared;

 private:
  std::shared_ptr<Shared> shared_;
};

}  // namespace manyforge::core
