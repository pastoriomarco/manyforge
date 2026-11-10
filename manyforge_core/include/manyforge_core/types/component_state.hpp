#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// Component model v2: the one view of a component.
/// Adapters produce it, the core holds and publishes it, everything else reads
/// it. Plain values; every enum has a wire name (`to_string`) and a parser.

enum class ComponentLifecycle : std::uint8_t {
  kOffline = 0,
  kNotPrepared,
  kPrepared,
  kEnabled,
  kFault,
};

enum class ComponentActivity : std::uint8_t {
  kIdle = 0,
  kBusy,
  kUnknown,
};

enum class StreamFreshness : std::uint8_t {
  kFresh = 0,
  kStale,
  kNone,
};

enum class Capability : std::uint8_t {
  kResetPosition = 0,
  kPowerOff,
  kReboot,
  kIoManualWrite,
  kIoOutputReadback,
  /// Identifies a simulator; its state never latches the cell or blocks Execute.
  kSimReset,
};

enum class ComponentAction : std::uint8_t {
  kPrepare = 0,
  kEnable,
  kDisable,
  kRecover,
  kResetPosition,
  kPowerOff,
  kReboot,
  kSimReset,
};

enum class ActionOutcome : std::uint8_t {
  kSucceeded = 0,
  kFailed,
  kUnknown,
};

/// Vendor-neutral fault codes (plan, "Fault catalogue").
enum class FaultCode : std::uint8_t {
  kEstop = 0,
  kProtectiveStop,
  kSafeguardStop,
  kLimitViolation,
  kSelfCollision,
  kMotionInvalid,
  kDriveFault,
  kCommunication,
  kExternalControl,
  kStreamStale,
  kProcessExited,
  kVendorFault,
  kOwnerStalled,
};

enum class FaultOperatorAction : std::uint8_t {
  kEnable = 0,
  kRecover,
  kNone,
};

struct Fault final {
  FaultCode code{FaultCode::kVendorFault};
  std::string message;
  bool clearable{};
  FaultOperatorAction operator_action{FaultOperatorAction::kNone};
  std::optional<std::string> vendor_code;
  std::optional<std::string> vendor_text;
  /// On an owner's fault, the members it concerns (e.g.
  /// C33 the controller I/O member; an e-stop every member); empty otherwise.
  std::vector<std::string> concerns;
  std::int64_t since_ns{};

  friend bool operator==(const Fault&, const Fault&) = default;
};

enum class StopCode : std::uint8_t { kEstop = 0 };

/// An observed stop condition, distinct from a component fault.
struct ComponentStop final {
  StopCode code{StopCode::kEstop};
  std::string message;
  std::optional<std::string> vendor_code;
  std::optional<std::string> vendor_text;
  std::int64_t since_ns{};

  friend bool operator==(const ComponentStop&, const ComponentStop&) = default;
};

/// Why a component is offline or not prepared (e.g. `shutdown_by_operator`).
struct ComponentStateReason final {
  std::string code;
  std::string message;

  friend bool operator==(const ComponentStateReason&, const ComponentStateReason&) = default;
};

struct ComponentStream final {
  std::string stream_id;
  StreamFreshness freshness{StreamFreshness::kNone};
  std::optional<std::int64_t> age_ns;

  friend bool operator==(const ComponentStream&, const ComponentStream&) = default;
};

struct ComponentActionInProgress final {
  ComponentAction action{ComponentAction::kPrepare};
  std::int64_t started_at_ns{};

  friend bool operator==(const ComponentActionInProgress&,
                         const ComponentActionInProgress&) = default;
};

struct ActionError final {
  std::string code;
  std::string message;
  std::optional<std::string> vendor_code;
  std::optional<std::string> vendor_text;

  friend bool operator==(const ActionError&, const ActionError&) = default;
};

struct ActionPhaseResult final {
  std::string component_id;
  ComponentAction action{ComponentAction::kRecover};
  ActionOutcome outcome{ActionOutcome::kUnknown};
  std::optional<ActionError> error;

  friend bool operator==(const ActionPhaseResult&, const ActionPhaseResult&) = default;
};

struct ActionResult final {
  ComponentAction action{ComponentAction::kPrepare};
  ActionOutcome outcome{ActionOutcome::kUnknown};
  std::optional<ActionError> error;
  std::int64_t started_at_ns{};
  std::int64_t finished_at_ns{};
  std::vector<ActionPhaseResult> phases{};

  friend bool operator==(const ActionResult&, const ActionResult&) = default;
};

/// One adapter-reported fact shown with the component (e.g. the
/// controller's mode). Display only: no logic reads it.
struct ComponentDetail final {
  std::string name;
  std::string value;

  friend bool operator==(const ComponentDetail&, const ComponentDetail&) = default;
};

/// One endpoint the component's adapter publishes (e.g. the assistant's
/// `assistant_request` URL); `role_id` is the provider manifest's endpoint role.
struct ComponentEndpoint final {
  std::string role_id;
  std::string url;

  friend bool operator==(const ComponentEndpoint&, const ComponentEndpoint&) = default;
};

struct ComponentState final {
  std::string component_id;
  /// The machine (owner component) this component is a member
  /// of, and its member slot; both absent for an owner-less component.
  std::optional<std::string> owner_id;
  std::optional<std::string> role;
  ComponentLifecycle lifecycle{ComponentLifecycle::kOffline};
  /// Present exactly when `lifecycle == kFault`.
  std::optional<Fault> fault;
  std::optional<ComponentStateReason> reason;
  ComponentActivity activity{ComponentActivity::kUnknown};
  std::vector<ComponentStream> streams;
  std::vector<Capability> capabilities;
  std::optional<ComponentActionInProgress> action;
  std::optional<ActionResult> last_action;
  /// Adapter-reported, display only; no logic may read it.
  std::vector<ComponentDetail> details;
  /// Adapter-published endpoints, present only while enabled; unlike
  /// `details`, logic may read it.
  std::vector<ComponentEndpoint> endpoints;
  std::int64_t observed_at_ns{};
  std::optional<ComponentStop> stop{};

  friend bool operator==(const ComponentState&, const ComponentState&) = default;
};

enum class CellLatchCauseKind : std::uint8_t {
  kFault = 0,
  kOffline,
  kActivityUnknown,
  kStop,
};

struct CellLatchCause final {
  std::string component_id;
  CellLatchCauseKind kind{CellLatchCauseKind::kFault};
  std::optional<Fault> fault;
  std::optional<ComponentStop> stop{};

  friend bool operator==(const CellLatchCause&, const CellLatchCause&) = default;
};

/// Derived from component states, never stored.
struct CellLatch final {
  bool latched{};
  std::vector<CellLatchCause> causes;

  friend bool operator==(const CellLatch&, const CellLatch&) = default;
};

/// A component is healthy only when it is neither faulted, offline nor stopped.
[[nodiscard]] bool component_healthy(const ComponentState& state) noexcept;

/// The reason code of a member shown `offline` while its
/// owner isn't usable (`offline`, `not_prepared` or `fault`).
inline constexpr std::string_view kOwnerUnavailableReason = "owner_unavailable";
inline constexpr std::string_view kStoppedByEstopReason = "stopped_by_estop";

/// The reason codes of a component `offline` by the operator's own power
/// action (power off, reboot). Such a component isn't a latch cause.
inline constexpr std::string_view kShutdownByOperatorReason = "shutdown_by_operator";
inline constexpr std::string_view kComponentRebootingReason = "component_rebooting";

/// Whether an owner in `owner` state leaves its members usable: only a
/// `prepared` (or `enabled`) owner without a stop does.
[[nodiscard]] bool owner_usable(const ComponentState& owner) noexcept;

/// `member` as published while its owner is in `owner`
/// state: unchanged while the owner is usable. A stopped, otherwise available
/// owner presents a healthy member as Prepared with a stopped reason; an
/// independent member fault/offline remains visible. Other unavailable owners
/// present members offline with reason `owner_unavailable`.
[[nodiscard]] ComponentState presented_member_state(ComponentState member,
                                                    const ComponentState& owner);

/// Whether `state` is outside the cell latch: a simulator declaring
/// `sim_reset`, `offline` by the operator's own power action, or a member
/// shown offline through an owner that is `not_prepared` or offline by the
/// operator's own power action. An exempt component neither adds a latch
/// cause nor keeps a latch from clearing.
[[nodiscard]] bool latch_exempt(const ComponentState& state,
                                const std::vector<ComponentState>& states);

/// Every latch condition of `states`, in their order: stop, `fault`, `offline`,
/// or `activity: unknown`. An independent stop and fault may both be present.
/// Components exempt by `latch_exempt` are no cause. A member shown offline
/// through its owner isn't a cause of its own: its owner is, once, with the
/// owner's fault, or `offline` when it is unexpectedly
/// offline.
[[nodiscard]] std::vector<CellLatchCause> cell_latch_causes(
    const std::vector<ComponentState>& states);

[[nodiscard]] std::string_view to_string(ComponentLifecycle value) noexcept;
[[nodiscard]] std::string_view to_string(ComponentActivity value) noexcept;
[[nodiscard]] std::string_view to_string(StreamFreshness value) noexcept;
[[nodiscard]] std::string_view to_string(Capability value) noexcept;
[[nodiscard]] std::string_view to_string(ComponentAction value) noexcept;
[[nodiscard]] std::string_view to_string(ActionOutcome value) noexcept;
[[nodiscard]] std::string_view to_string(FaultCode value) noexcept;
[[nodiscard]] std::string_view to_string(StopCode value) noexcept;
[[nodiscard]] std::string_view to_string(FaultOperatorAction value) noexcept;
[[nodiscard]] std::string_view to_string(CellLatchCauseKind value) noexcept;

[[nodiscard]] std::optional<ComponentLifecycle> parse_component_lifecycle(
    std::string_view wire) noexcept;
[[nodiscard]] std::optional<ComponentActivity> parse_component_activity(
    std::string_view wire) noexcept;
[[nodiscard]] std::optional<StreamFreshness> parse_stream_freshness(std::string_view wire) noexcept;
[[nodiscard]] std::optional<Capability> parse_capability(std::string_view wire) noexcept;
[[nodiscard]] std::optional<ComponentAction> parse_component_action(
    std::string_view wire) noexcept;
[[nodiscard]] std::optional<ActionOutcome> parse_action_outcome(std::string_view wire) noexcept;
[[nodiscard]] std::optional<FaultCode> parse_fault_code(std::string_view wire) noexcept;
[[nodiscard]] std::optional<StopCode> parse_stop_code(std::string_view wire) noexcept;
[[nodiscard]] std::optional<FaultOperatorAction> parse_fault_operator_action(
    std::string_view wire) noexcept;
[[nodiscard]] std::optional<CellLatchCauseKind> parse_cell_latch_cause_kind(
    std::string_view wire) noexcept;

}  // namespace manyforge::core
