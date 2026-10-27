#pragma once

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// CP9 component stream health (plan `component-liveness-and-interface-2026-10-26`).
/// Adapters declare streams and push samples; the CCLC reacts by severity.

enum class StreamSeverityDTO : std::uint8_t {
  /// Lost component: motion blocked; after the target was reached a
  /// `provider_lost` incident (Full Stop, alarm, Recover).
  kFault = 0,
  /// New motion of the component blocked; no incident.
  kBlocking,
  /// Shown only.
  kWarning,
};

enum class StreamRecoveryDTO : std::uint8_t {
  /// A live stream that went stale holds its condition until the operator's
  /// Recover of the component. `fault` always behaves as `ack`.
  kAck = 0,
  /// The condition clears when the stream is live again.
  kAuto,
};

enum class StreamTimestampSourceDTO : std::uint8_t {
  /// A sample advances only when its source stamp increases.
  kMessageStamp = 0,
  /// Every sample advances (no usable stamp).
  kArrival,
};

enum class StreamLivenessStateDTO : std::uint8_t { kLive = 0, kStale, kAbsent };

[[nodiscard]] std::string_view to_string(StreamSeverityDTO value) noexcept;
[[nodiscard]] std::string_view to_string(StreamRecoveryDTO value) noexcept;
[[nodiscard]] std::string_view to_string(StreamTimestampSourceDTO value) noexcept;
[[nodiscard]] std::string_view to_string(StreamLivenessStateDTO value) noexcept;
[[nodiscard]] std::optional<StreamSeverityDTO> parse_stream_severity(std::string_view value) noexcept;
[[nodiscard]] std::optional<StreamRecoveryDTO> parse_stream_recovery(std::string_view value) noexcept;

/// Typed condition codes (`StreamHealthDTO::error.code`, Kernel refusal
/// `details.reason`, incident diagnostic code).
inline constexpr std::string_view kComponentStreamStaleCode = "component_stream_stale";
inline constexpr std::string_view kComponentStreamAbsentCode = "component_stream_absent";
inline constexpr std::string_view kComponentStreamAcknowledgementRequiredCode =
    "component_stream_acknowledgement_required";

inline constexpr std::size_t kMaxStreamIdBytes = 64U;
inline constexpr std::size_t kMaxStreamFaultCodeBytes = 64U;
inline constexpr std::size_t kMaxStreamFaultDetailBytes = 256U;

/// 1..64 bytes of `[a-z0-9_./-]`.
[[nodiscard]] bool ValidStreamId(std::string_view value) noexcept;
/// 1..64 bytes of `[a-z0-9_.]`.
[[nodiscard]] bool ValidStreamFaultCode(std::string_view value) noexcept;

/// One declared stream with its adapter-kind defaults.
struct StreamDeclarationDTO final {
  std::string stream_id;
  std::int64_t budget_ns{};
  StreamSeverityDTO severity{StreamSeverityDTO::kFault};
  StreamRecoveryDTO recovery{StreamRecoveryDTO::kAck};
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
  std::optional<StreamSeverityDTO> severity;
  std::optional<StreamRecoveryDTO> recovery;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !controllable_component_id.empty() && ValidStreamId(stream_id) &&
           (budget_ns || severity || recovery) && (!budget_ns || *budget_ns > 0);
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
  StreamRecoveryDTO recovery{StreamRecoveryDTO::kAck};
  StreamTimestampSourceDTO timestamp_source{StreamTimestampSourceDTO::kMessageStamp};
  bool acknowledgement_required{};
  /// Active condition (stale/absent/acknowledgement) or pending fault event.
  std::optional<ErrorDTO> error;

  friend bool operator==(const StreamHealthDTO&, const StreamHealthDTO&) = default;
};

/// What the CCLC acts on for one component. Errors carry no age, so a
/// verdict changes only on a transition.
struct ComponentStreamVerdictDTO final {
  /// A `fault` stream is stale or absent: incident diagnostic once reached.
  std::optional<ErrorDTO> fault_loss;
  /// The first pending adapter fault event (kept until Recover).
  std::optional<ErrorDTO> fault_event;
  /// Kernel refusal for new motion of the component (`fault`/`blocking`).
  std::optional<ErrorDTO> motion_hold;

  [[nodiscard]] bool empty() const noexcept { return !fault_loss && !fault_event && !motion_hold; }
  friend bool operator==(const ComponentStreamVerdictDTO&,
                         const ComponentStreamVerdictDTO&) = default;
};

class ComponentStreamHealth;

/// Adapter handle of one declared stream. Destruction undeclares it.
/// `Record*` are lock-free and allocation-free unless the stream was not
/// live (one transition notification). The registry's clock must outlive it.
class StreamRecorder final {
 public:
  ~StreamRecorder();
  StreamRecorder(const StreamRecorder&) = delete;
  StreamRecorder& operator=(const StreamRecorder&) = delete;

  /// One sample with its source stamp (ns, any clock that increases).
  /// For `arrival` streams the stamp is ignored.
  void Record(std::int64_t source_stamp_ns) noexcept;
  /// One sample without a usable stamp.
  void RecordArrival() noexcept;
  /// Immediate bad news (vendor fault, emergency stop, invalid values).
  /// Invalid codes become `component_stream_fault`; detail is truncated.
  void ReportFault(std::string_view code, std::string_view detail) noexcept;

  [[nodiscard]] const ControllableComponentId& component() const noexcept;
  [[nodiscard]] const std::string& stream_id() const noexcept;

  struct Slot;

 private:
  friend class ComponentStreamHealth;
  explicit StreamRecorder(std::shared_ptr<Slot> slot) noexcept;
  std::shared_ptr<Slot> slot_;
};

/// CCLC-side registry of every declared stream of one process (CP9 §3).
///
/// One deadline timer for the earliest deadline (a heap of lazily refreshed
/// deadlines; no polling, no scan of all streams). All classification and
/// every listener call run on the scheduler's Monitoring lane; mutating
/// calls only mark the component and schedule that evaluation.
class ComponentStreamHealth final {
 public:
  using Listener =
      std::function<void(const ControllableComponentId&, const ComponentStreamVerdictDTO&)>;

  ComponentStreamHealth(IClock& clock, IScheduler& scheduler);
  ~ComponentStreamHealth();
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
  bool ReportDeclaredFault(const ControllableComponentId& component, std::string_view stream_id,
                           std::string_view code, std::string_view detail);

  /// Replaces every Deployment override (product install). Invalid rows are
  /// refused as a whole and nothing changes.
  [[nodiscard]] Result<void> SetOverrides(std::vector<StreamParameterOverrideDTO> overrides);

  /// Current health of every stream of the component, sorted by stream id.
  [[nodiscard]] std::vector<StreamHealthDTO> Read(const ControllableComponentId& component) const;
  /// Current verdict (the same value the listener last received or will
  /// receive after a pending evaluation).
  [[nodiscard]] ComponentStreamVerdictDTO Verdict(const ControllableComponentId& component) const;

  /// The operator's Recover of the component: clears `ack` latches and
  /// pending fault events. True when an `ack` latch was cleared.
  bool Acknowledge(const ControllableComponentId& component);

  /// One listener (the CCLC composition). Replaces the previous one and
  /// replays every non-empty verdict. Returns a token for `ClearListener`.
  std::uint64_t SetListener(Listener listener);
  /// Removes the listener named by `token` and waits for a running call.
  void ClearListener(std::uint64_t token) noexcept;
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
