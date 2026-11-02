#include <algorithm>
#include <array>
#include <manyforge_core/types/component_state.hpp>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Enum, std::size_t Size>
using WireNames = std::array<std::pair<Enum, std::string_view>, Size>;

constexpr WireNames<ComponentLifecycle, 5U> kLifecycleNames{{
    {ComponentLifecycle::kOffline, "offline"},
    {ComponentLifecycle::kNotPrepared, "not_prepared"},
    {ComponentLifecycle::kPrepared, "prepared"},
    {ComponentLifecycle::kEnabled, "enabled"},
    {ComponentLifecycle::kFault, "fault"},
}};

constexpr WireNames<ComponentActivity, 3U> kActivityNames{{
    {ComponentActivity::kIdle, "idle"},
    {ComponentActivity::kBusy, "busy"},
    {ComponentActivity::kUnknown, "unknown"},
}};

constexpr WireNames<StreamFreshness, 3U> kFreshnessNames{{
    {StreamFreshness::kFresh, "fresh"},
    {StreamFreshness::kStale, "stale"},
    {StreamFreshness::kNone, "none"},
}};

constexpr WireNames<Capability, 6U> kCapabilityNames{{
    {Capability::kResetPosition, "reset_position"},
    {Capability::kPowerOff, "power_off"},
    {Capability::kReboot, "reboot"},
    {Capability::kIoManualWrite, "io_manual_write"},
    {Capability::kIoOutputReadback, "io_output_readback"},
    {Capability::kSimReset, "sim_reset"},
}};

constexpr WireNames<ComponentAction, 8U> kActionNames{{
    {ComponentAction::kPrepare, "prepare"},
    {ComponentAction::kEnable, "enable"},
    {ComponentAction::kDisable, "disable"},
    {ComponentAction::kRecover, "recover"},
    {ComponentAction::kResetPosition, "reset_position"},
    {ComponentAction::kPowerOff, "power_off"},
    {ComponentAction::kReboot, "reboot"},
    {ComponentAction::kSimReset, "sim_reset"},
}};

constexpr WireNames<ActionOutcome, 3U> kOutcomeNames{{
    {ActionOutcome::kSucceeded, "succeeded"},
    {ActionOutcome::kFailed, "failed"},
    {ActionOutcome::kUnknown, "unknown"},
}};

constexpr WireNames<FaultCode, 12U> kFaultCodeNames{{
    {FaultCode::kEstop, "fault_estop"},
    {FaultCode::kProtectiveStop, "fault_protective_stop"},
    {FaultCode::kSafeguardStop, "fault_safeguard_stop"},
    {FaultCode::kLimitViolation, "fault_limit_violation"},
    {FaultCode::kSelfCollision, "fault_self_collision"},
    {FaultCode::kMotionInvalid, "fault_motion_invalid"},
    {FaultCode::kDriveFault, "fault_drive"},
    {FaultCode::kCommunication, "fault_communication"},
    {FaultCode::kExternalControl, "fault_external_control"},
    {FaultCode::kStreamStale, "fault_stream_stale"},
    {FaultCode::kProcessExited, "fault_process_exited"},
    {FaultCode::kVendorFault, "fault_vendor"},
}};

constexpr WireNames<FaultOperatorAction, 3U> kOperatorActionNames{{
    {FaultOperatorAction::kEnable, "enable"},
    {FaultOperatorAction::kRecover, "recover"},
    {FaultOperatorAction::kNone, "none"},
}};

constexpr WireNames<CellLatchCauseKind, 3U> kCauseKindNames{{
    {CellLatchCauseKind::kFault, "fault"},
    {CellLatchCauseKind::kOffline, "offline"},
    {CellLatchCauseKind::kActivityUnknown, "activity_unknown"},
}};

template <typename Enum, std::size_t Size>
[[nodiscard]] std::string_view name_of(const WireNames<Enum, Size>& names,
                                       const Enum value) noexcept {
  for (const auto& [candidate, name] : names)
    if (candidate == value) return name;
  return "unknown";
}

template <typename Enum, std::size_t Size>
[[nodiscard]] std::optional<Enum> parse(const WireNames<Enum, Size>& names,
                                        const std::string_view wire) noexcept {
  for (const auto& [value, name] : names)
    if (name == wire) return value;
  return std::nullopt;
}

}  // namespace

bool component_healthy(const ComponentState& state) noexcept {
  return state.lifecycle != ComponentLifecycle::kFault &&
         state.lifecycle != ComponentLifecycle::kOffline;
}

bool owner_usable(const ComponentState& owner) noexcept {
  return owner.lifecycle == ComponentLifecycle::kPrepared ||
         owner.lifecycle == ComponentLifecycle::kEnabled;
}

ComponentState presented_member_state(ComponentState member, const ComponentState& owner) {
  if (owner_usable(owner)) return member;
  const auto why = owner.reason   ? owner.reason->message
                   : owner.fault  ? owner.fault->message
                                  : std::string("not prepared");
  member.lifecycle = ComponentLifecycle::kOffline;
  member.fault.reset();
  member.reason = ComponentStateReason{std::string(kOwnerUnavailableReason),
                                       "through " + owner.component_id + ": " + why};
  return member;
}

namespace {

[[nodiscard]] bool offline_by_operator(const ComponentState& state) noexcept {
  return state.lifecycle == ComponentLifecycle::kOffline && state.reason &&
         (state.reason->code == kShutdownByOperatorReason ||
          state.reason->code == kComponentRebootingReason);
}

[[nodiscard]] bool through_owner(const ComponentState& state) noexcept {
  return state.owner_id && state.reason && state.reason->code == kOwnerUnavailableReason;
}

[[nodiscard]] const ComponentState* owner_of(const ComponentState& member,
                                             const std::vector<ComponentState>& states) {
  const auto owner = std::find_if(states.begin(), states.end(), [&](const auto& row) {
    return row.component_id == *member.owner_id;
  });
  return owner == states.end() ? nullptr : &*owner;
}

}  // namespace

bool latch_exempt(const ComponentState& state, const std::vector<ComponentState>& states) {
  if (offline_by_operator(state)) return true;
  if (!through_owner(state)) return false;
  const auto* owner = owner_of(state, states);
  return owner != nullptr && (owner->lifecycle == ComponentLifecycle::kNotPrepared ||
                              offline_by_operator(*owner));
}

std::vector<CellLatchCause> cell_latch_causes(const std::vector<ComponentState>& states) {
  std::vector<CellLatchCause> causes;
  const auto listed = [&](const std::string& id) {
    return std::any_of(causes.begin(), causes.end(),
                       [&](const auto& cause) { return cause.component_id == id; });
  };
  for (const auto& state : states) {
    if (latch_exempt(state, states)) continue;
    if (through_owner(state)) {
      // One cause, one entry: the owner, not its members (CONTRACTS §5, §16).
      if (listed(*state.owner_id)) continue;
      const auto* owner = owner_of(state, states);
      if (owner != nullptr && owner->lifecycle == ComponentLifecycle::kFault)
        causes.push_back({owner->component_id, CellLatchCauseKind::kFault, owner->fault});
      else
        causes.push_back({*state.owner_id, CellLatchCauseKind::kOffline, std::nullopt});
      continue;
    }
    if (listed(state.component_id)) continue;
    if (state.lifecycle == ComponentLifecycle::kFault) {
      causes.push_back({state.component_id, CellLatchCauseKind::kFault, state.fault});
    } else if (state.lifecycle == ComponentLifecycle::kOffline) {
      causes.push_back({state.component_id, CellLatchCauseKind::kOffline, std::nullopt});
    } else if (state.activity == ComponentActivity::kUnknown) {
      causes.push_back({state.component_id, CellLatchCauseKind::kActivityUnknown, std::nullopt});
    }
  }
  return causes;
}

std::string_view to_string(const ComponentLifecycle value) noexcept {
  return name_of(kLifecycleNames, value);
}
std::string_view to_string(const ComponentActivity value) noexcept {
  return name_of(kActivityNames, value);
}
std::string_view to_string(const StreamFreshness value) noexcept {
  return name_of(kFreshnessNames, value);
}
std::string_view to_string(const Capability value) noexcept {
  return name_of(kCapabilityNames, value);
}
std::string_view to_string(const ComponentAction value) noexcept {
  return name_of(kActionNames, value);
}
std::string_view to_string(const ActionOutcome value) noexcept {
  return name_of(kOutcomeNames, value);
}
std::string_view to_string(const FaultCode value) noexcept {
  return name_of(kFaultCodeNames, value);
}
std::string_view to_string(const FaultOperatorAction value) noexcept {
  return name_of(kOperatorActionNames, value);
}
std::string_view to_string(const CellLatchCauseKind value) noexcept {
  return name_of(kCauseKindNames, value);
}

std::optional<ComponentLifecycle> parse_component_lifecycle(const std::string_view wire) noexcept {
  return parse(kLifecycleNames, wire);
}
std::optional<ComponentActivity> parse_component_activity(const std::string_view wire) noexcept {
  return parse(kActivityNames, wire);
}
std::optional<StreamFreshness> parse_stream_freshness(const std::string_view wire) noexcept {
  return parse(kFreshnessNames, wire);
}
std::optional<Capability> parse_capability(const std::string_view wire) noexcept {
  return parse(kCapabilityNames, wire);
}
std::optional<ComponentAction> parse_component_action(const std::string_view wire) noexcept {
  return parse(kActionNames, wire);
}
std::optional<ActionOutcome> parse_action_outcome(const std::string_view wire) noexcept {
  return parse(kOutcomeNames, wire);
}
std::optional<FaultCode> parse_fault_code(const std::string_view wire) noexcept {
  return parse(kFaultCodeNames, wire);
}
std::optional<FaultOperatorAction> parse_fault_operator_action(
    const std::string_view wire) noexcept {
  return parse(kOperatorActionNames, wire);
}
std::optional<CellLatchCauseKind> parse_cell_latch_cause_kind(
    const std::string_view wire) noexcept {
  return parse(kCauseKindNames, wire);
}

}  // namespace manyforge::core
