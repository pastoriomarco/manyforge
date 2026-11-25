// The gateway core's command tables and its one client (Gateway): wire
// envelopes, attribution and the scene commit's cross-field rules, as the
// Python clients sent them (`manyforge_headless_api/execution.py`,
// `cell_admission.py`, `manager.py`, `service.py` scene_commit,
// `manyforge_program_runner/execution_transport.py`,
// `manyforge_ros/manager_scene_runtime.py`).

#include <array>
#include <string>
#include <utility>

#include "manyforge_headless/gateway_core.hpp"

namespace manyforge::headless::gateway {

namespace ci = manyforge::content_identity;

namespace {

constexpr std::size_t kMaximumPrincipalBytes = 256U;

using L = HostLane;
using A = Attribution;

constexpr std::string_view kCellAdmission = kCellAdmissionResponseApiVersion;

// One row per wire command; `reply_names_command` where a Python client
// checked that a successful reply names its command; the reply version where
// the host answers with another than v6 (its cell-admission codec,
// manyforge_roboplan/src/cell_admission_command_codec.hpp).
const std::vector<HostCommandSpec> kHostCommands{
    // execution-command.sock: projections and reads.
    {"status", L::kCommand, A::kNone, true},
    {"identity", L::kCommand, A::kNone, true},
    {"targets", L::kCommand, A::kNone, true},
    {"control_state", L::kCommand, A::kNone, true},
    {"observation", L::kCommand, A::kNone, true},
    {"query_operation", L::kCommand, A::kNone, true},
    {"query_target", L::kCommand, A::kNone, true},
    {"events", L::kCommand, A::kNone, true},
    {"read", L::kCommand, A::kNone, true},
    {"read_volatile_motion_trace", L::kCommand, A::kCaller, true},
    {"get_operation_recovery", L::kCommand, A::kNone, true},
    {"get_runtime_scale_control", L::kCommand, A::kNone, true},
    {"read_program_execution_scope", L::kCommand, A::kNone, false},
    {"resolve_attach_active_tcp", L::kCommand, A::kNone, true},
    {"read_active_tcp", L::kCommand, A::kNone, true},
    {"reset_robot_position_capability", L::kCommand, A::kNone, true},
    // execution-command.sock: effects.
    {"submit", L::kCommand, A::kCaller, true},
    {"read_manual_handover", L::kCommand, A::kCaller, true},
    {"preview_motion", L::kCommand, A::kCaller, true},
    {"preview_capability_command", L::kCommand, A::kCaller, true},
    {"apply_operation_recovery", L::kCommand, A::kNone, true},
    {"plan_pick", L::kCommand, A::kCaller, true},
    {"plan_place", L::kCommand, A::kCaller, true},
    {"reset", L::kCommand, A::kCaller, true},
    {"acknowledge_completed", L::kCommand, A::kCaller, true},
    {"set_io_output", L::kCommand, A::kNone, true},
    {"set_mock_io_input", L::kCommand, A::kFixedInternal, true},
    {"release_halt", L::kCommand, A::kInternalOnly, true},
    {"reset_robot_position", L::kCommand, A::kNone, true},
    {"reset_active_tcp", L::kCommand, A::kCaller, true},
    {"register_program_run_capture", L::kCommand, A::kNone, true},
    {"register_io_watch", L::kCommand, A::kCaller, true},
    {"read_io_watch", L::kCommand, A::kCaller, true},
    {"release_io_watch", L::kCommand, A::kCaller, true},
    {"start_mock_io_node_simulation", L::kCommand, A::kCaller, true},
    {"end_mock_io_node_simulation", L::kCommand, A::kCaller, true},
    {"set_mock_io_run_paused", L::kCommand, A::kCaller, true},
    {"publish_program_bbkeys", L::kCommand, A::kCaller, true},
    {"release_program_selection", L::kCommand, A::kCaller, false},
    {"start_program_selection", L::kCommand, A::kCaller, false},
    {"check_program_node_motion_attention", L::kCommand, A::kCaller, false},
    {"set_rule_active", L::kCommand, A::kNone, false},
    {"reset_rule_activation", L::kCommand, A::kNone, false},
    {"clear_outside_change", L::kCommand, A::kNone, false},
    {"cell_admission_get", L::kCommand, A::kNone, true, kCellAdmission},
    {"cell_admission_acquire", L::kCommand, A::kNone, true, kCellAdmission},
    {"cell_admission_release", L::kCommand, A::kNone, true, kCellAdmission},
    {"cell_admission_recovery_receipt", L::kCommand, A::kNone, true,
     kCellAdmission},
    // execution-control.sock: Stop and the memory-only reads beside it.
    {"stop", L::kControl, A::kCaller, true},
    {"stop_all", L::kControl, A::kCaller, true},
    {"halt", L::kControl, A::kCaller, true},
    {"cell_stop", L::kControl, A::kInternalOnly, true},
    {"recognized_stop", L::kControl, A::kCaller, true},
    {"stop_program_run", L::kControl, A::kInternalOnly, true},
    {"stop_program_selection", L::kControl, A::kCaller, true},
    {"read_stop", L::kControl, A::kInternalOnly, true},
    {"set_runtime_scale_control", L::kControl, A::kCaller, true},
    {"read_planning_job", L::kControl, A::kNone, true},
    {"cancel_planning_job", L::kControl, A::kNone, true},
    {"release_planning_job", L::kControl, A::kNone, true},
    // execution-component.sock.
    {"read_component_model", L::kComponent, A::kNone, true},
    {"component_action", L::kComponent, A::kNone, true},
    {"component_recovery_preflight", L::kComponent, A::kNone, true},
    {"cell_admission_publish_operator_alarm_attention", L::kComponent,
     A::kNone, true, kCellAdmission},
};

const std::vector<ManagerCommandSpec> kManagerCommands{
    // The runner's narrow scene read at the owner (D1).
    {"scene_owner_read"},
    {"scene_runtime_owner_read"},
    {"scene_runtime_latch_between_nodes"},
    {"scene_runtime_observer_open"},
    {"scene_runtime_observe"},
    {"scene_transaction_status"},
    {"scene_pose_query"},
    {"deployment_candidate_preview"},
    {"apply"},
    {"apply_status"},
    {"apply_acknowledge"},
    {"apply_release_for_new"},
};

CanonicalValue Text(std::string_view text) {
  return CanonicalValue(std::string(text));
}

/// The caller's principal: bounded non-empty NFC text (`_request_attribution`,
/// `commit_scene_transaction`).
bool ValidPrincipal(const std::string &principal) {
  return !principal.empty() && principal.size() <= kMaximumPrincipalBytes &&
         ci::is_utf8_nfc(principal);
}

CanonicalValue AttributionValue(std::string_view transport,
                                const std::optional<std::string> &principal) {
  return CanonicalValue(CanonicalValue::Object{
      {"assurance", Text(principal.has_value() ? "externally_authenticated"
                                               : "unverified")},
      {"principal_id",
       principal.has_value() ? CanonicalValue(*principal) : CanonicalValue()},
      {"transport", Text(transport)},
  });
}

[[noreturn]] void AttributionInvalid(std::string message) {
  throw GatewayError("execution_request_attribution_invalid",
                     std::move(message));
}

/// The `attribution` member of one host command, or nullopt when the
/// command carries none.
std::optional<CanonicalValue> HostAttribution(
    const HostCommandSpec &spec, AttributionTransport transport,
    const std::optional<std::string> &principal) {
  if (spec.attribution == A::kInternalOnly &&
      transport != AttributionTransport::kInternal)
    AttributionInvalid(std::string(spec.name) +
                       " is available only to the internal owner transport");
  if (principal.has_value() && !ValidPrincipal(*principal))
    AttributionInvalid(
        "trusted execution principal must be bounded non-empty NFC text");
  switch (spec.attribution) {
    case A::kNone:
      if (principal.has_value())
        AttributionInvalid(std::string(spec.name) +
                           " carries no request attribution");
      return std::nullopt;
    case A::kFixedInternal:
      if (principal.has_value())
        AttributionInvalid(std::string(spec.name) +
                           " carries the internal attribution only");
      return AttributionValue("internal", std::nullopt);
    case A::kInternalOnly:
    case A::kCaller:
      if (transport == AttributionTransport::kInternal) {
        if (principal.has_value())
          AttributionInvalid(
              "internal execution requests cannot claim an external principal");
        return AttributionValue("internal", std::nullopt);
      }
      return AttributionValue("http", principal);
  }
  return std::nullopt;
}

/// Builds {envelope..., ...members}; a member may not repeat an envelope
/// member.
CanonicalValue Envelope(CanonicalValue::Object envelope,
                        CanonicalValue::Object members,
                        const char *invalid_code) {
  for (auto &member : members) {
    for (const auto &[name, _] : envelope)
      if (member.first == name)
        throw GatewayError(invalid_code, "the request member '" + name +
                                             "' is set by the gateway");
    envelope.push_back(std::move(member));
  }
  try {
    return CanonicalValue(std::move(envelope));
  } catch (const ci::CanonicalValueError &error) {
    throw GatewayError(invalid_code,
                       std::string("command is not canonically encodable: ") +
                           error.what());
  }
}

const CanonicalValue *Find(const CanonicalValue::Object &object,
                           std::string_view name) {
  for (const auto &[member, value] : object)
    if (member == name) return &value;
  return nullptr;
}

bool IsNullOrAbsent(const CanonicalValue *value) {
  return value == nullptr || value->is_null();
}

/// Drops `name` when it is null (`_transaction_body`).
void EraseNullMember(CanonicalValue::Object &object, std::string_view name) {
  for (auto it = object.begin(); it != object.end(); ++it)
    if (it->first == name) {
      if (it->second.is_null()) object.erase(it);
      return;
    }
}

}  // namespace

std::string_view HostLaneSocketFile(HostLane lane) noexcept {
  switch (lane) {
    case HostLane::kCommand:
      return "execution-command.sock";
    case HostLane::kControl:
      return "execution-control.sock";
    case HostLane::kComponent:
      return "execution-component.sock";
  }
  return "execution-command.sock";
}

const std::vector<HostCommandSpec> &HostCommands() { return kHostCommands; }
const std::vector<ManagerCommandSpec> &ManagerCommands() {
  return kManagerCommands;
}

const HostCommandSpec *FindHostCommand(std::string_view name) {
  for (const auto &spec : kHostCommands)
    if (spec.name == name) return &spec;
  return nullptr;
}

const ManagerCommandSpec *FindManagerCommand(std::string_view name) {
  for (const auto &spec : kManagerCommands)
    if (spec.name == name) return &spec;
  return nullptr;
}

Gateway::Gateway(GatewayConfig config) : config_(std::move(config)) {
  if (config_.execution_run_root.has_value())
    execution_root_ = std::make_unique<PinnedRunRoot>(
        *config_.execution_run_root, "execution_owner_unavailable");
  if (config_.manager_run_root.has_value())
    manager_root_ = std::make_unique<PinnedRunRoot>(
        *config_.manager_run_root, "manager_command_unavailable");
}

Gateway::~Gateway() = default;

CanonicalValue Gateway::Host(std::string_view command,
                             CanonicalValue::Object members,
                             std::optional<std::string> principal_id) const {
  const auto *spec = FindHostCommand(command);
  if (spec == nullptr)
    throw GatewayError("execution_command_invalid",
                       "execution command '" + std::string(command) +
                           "' is not a gateway command");
  CanonicalValue::Object envelope{
      {"api_version", Text(kExecutionCommandApiVersion)},
      {"command", Text(spec->name)},
  };
  if (auto attribution =
          HostAttribution(*spec, config_.attribution, principal_id))
    envelope.emplace_back("attribution", std::move(*attribution));
  if (!execution_root_)
    throw GatewayError("execution_owner_unavailable",
                       "the execution host run root is not configured");
  const auto request = Envelope(std::move(envelope), std::move(members),
                                "execution_command_invalid");
  auto response =
      Exchange(*execution_root_, HostLaneSocketFile(spec->lane),
               OwnerProtocol::kExecutionHost, spec->response_version, request,
               config_.connect_limit);
  if (spec->reply_names_command &&
      response.find_object_member("ok")->as_bool()) {
    const auto *named = response.find_object_member("command");
    if (named == nullptr || !named->is_string() ||
        named->as_string() != spec->name)
      throw GatewayError("execution_command_response_invalid",
                         "execution " + std::string(spec->name) +
                             " response has the wrong command kind");
  }
  return response;
}

CanonicalValue Gateway::Manager(
    std::string_view command, CanonicalValue::Object members,
    const std::optional<std::chrono::milliseconds> limit) const {
  if (FindManagerCommand(command) == nullptr)
    throw GatewayError("manager_command_invalid",
                       "Manager command '" + std::string(command) +
                           "' is not a gateway command");
  if (!manager_root_)
    throw GatewayError("manager_command_unavailable",
                       "the Manager run root is not configured");
  const auto request = Envelope(
      CanonicalValue::Object{{"api_version", Text(kManagerCommandApiVersion)},
                             {"command", Text(command)}},
      std::move(members), "manager_command_invalid");
  return Exchange(*manager_root_, kManagerSocketFile, OwnerProtocol::kManager,
                  kManagerResponseApiVersion, request, config_.connect_limit,
                  limit);
}

CanonicalValue Gateway::SceneCommit(
    CanonicalValue::Object transaction,
    std::optional<std::string> principal_id) const {
  // Cross-field rules of HAPI service.scene_commit, before anything is sent.
  static constexpr std::array<std::string_view, 5> kAuthoredKinds{
      "add_object", "upsert_object", "remove_object", "update_object_pose",
      "set_collision_policy"};
  bool authored = false;
  if (const auto *operation = Find(transaction, "operation");
      operation != nullptr && operation->is_object())
    if (const auto *kind = operation->find_object_member("kind");
        kind != nullptr && kind->is_string())
      for (const auto candidate : kAuthoredKinds)
        authored = authored || kind->as_string() == candidate;
  const bool evidence = !IsNullOrAbsent(Find(transaction, "program_node_evidence"));
  const bool admission =
      !IsNullOrAbsent(Find(transaction, "expected_admission_generation"));
  const bool reset =
      !IsNullOrAbsent(Find(transaction, "program_reset_scene_authorization"));
  if (evidence) {
    if (!admission || reset)
      throw GatewayError("invalid_request",
                         "runtime Scene transaction needs exact child admission");
  } else if (authored) {
    if (admission == reset)
      throw GatewayError(
          "authoring_not_ready",
          "authored-scene mutation requires exactly one ordinary or Program "
          "Reset authorization");
  } else if (reset) {
    throw GatewayError(
        "invalid_request",
        "runtime scene mutation cannot carry Program Reset authorization");
  }

  // The attribution (manager.commit_scene_transaction): always the HTTP
  // form, as every scene commit reached the Manager through Headless's
  // route; an internal caller (the Program runner) has no principal.
  if (principal_id.has_value() && !ValidPrincipal(*principal_id))
    throw GatewayError(
        "scene_request_attribution_invalid",
        "trusted scene principal must be bounded non-empty NFC text");
  if (config_.attribution == AttributionTransport::kInternal &&
      principal_id.has_value())
    throw GatewayError(
        "scene_request_attribution_invalid",
        "internal scene requests cannot claim an external principal");

  // The wire body (manager._transaction_body): absent optional members stay
  // absent, not null; the two nullable members are always present.
  EraseNullMember(transaction, "program_node_evidence");
  for (const auto name :
       {std::string_view("expected_admission_generation"),
        std::string_view("program_reset_scene_authorization")})
    if (Find(transaction, name) == nullptr)
      transaction.emplace_back(std::string(name), CanonicalValue());
  for (auto it = transaction.begin(); it != transaction.end(); ++it)
    if (it->first == "operation" && it->second.is_object()) {
      auto operation = it->second.as_object();
      for (const auto name : {"grasp_lost_bound", "on_grasp_lost", "mass_kg",
                              "carrier_component_id"})
        EraseNullMember(operation, name);
      it->second = CanonicalValue(std::move(operation));
    }
  for (auto it = transaction.begin(); it != transaction.end(); ++it)
    if (it->first == "attribution") {
      transaction.erase(it);
      break;
    }
  transaction.emplace_back("attribution",
                           AttributionValue("http", principal_id));

  if (!manager_root_)
    throw GatewayError("manager_command_unavailable",
                       "the Manager run root is not configured");
  CanonicalValue body;
  try {
    body = CanonicalValue(std::move(transaction));
  } catch (const ci::CanonicalValueError &error) {
    throw GatewayError("manager_command_invalid",
                       std::string("scene transaction is not canonically "
                                   "encodable: ") +
                           error.what());
  }
  const CanonicalValue request(CanonicalValue::Object{
      {"api_version", Text(kManagerCommandApiVersion)},
      {"command", Text("scene_commit")},
      {"transaction", std::move(body)},
  });
  return Exchange(*manager_root_, kManagerSocketFile, OwnerProtocol::kManager,
                  kManagerResponseApiVersion, request, config_.connect_limit);
}

}  // namespace manyforge::headless::gateway
