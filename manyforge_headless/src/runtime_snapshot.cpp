#include "manyforge_headless/runtime_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

#include "manyforge_headless/scene_runtime_state_owner.hpp"
#include "manyforge_headless/source_contract.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;

[[noreturn]] void Fail(const std::string &detail) {
  throw RuntimeSnapshotError("headless runtime snapshot: " + detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label,
                      const bool nonzero = false) {
  Require(value <= kMaximumSafeInteger && (!nonzero || value != 0U),
          std::string(label) + " must be an I-JSON safe integer");
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(const std::int64_t value, const std::string_view label) {
  Require(value >= 0, std::string(label) + " must be a nonnegative int64");
  return content_identity::nonnegative_int64_value(value);
}

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *value = root.find_object_member(name);
  Require(value != nullptr,
          "source payload is missing field " + std::string(name));
  return *value;
}

std::string RequiredString(const CanonicalValue &root,
                           const std::string_view name) {
  const auto *value =
      std::get_if<std::string>(&RequiredMember(root, name).storage());
  Require(value != nullptr && !value->empty(),
          "source payload field " + std::string(name) +
              " must be a non-empty string");
  return *value;
}

bool RequiredBool(const CanonicalValue &root, const std::string_view name) {
  const auto *value = std::get_if<bool>(&RequiredMember(root, name).storage());
  Require(value != nullptr,
          "source payload field " + std::string(name) + " must be a boolean");
  return *value;
}

// One closed `{controllable_component_id, simulator}` row per
// ROS-hosted component. Optional so older Manager publications stay readable.
void ValidateComponentSimulatorDependencies(const CanonicalValue &rows) {
  Require(rows.is_array(),
          "Manager component_simulator_dependencies must be an array");
  std::vector<std::string> components;
  components.reserve(rows.as_array().size());
  for (const auto &row : rows.as_array()) {
    Require(row.is_object() && row.as_object().size() == 2U,
            "Manager component simulator dependency is not the closed v1 shape");
    const auto simulator = RequiredString(row, "simulator");
    Require(simulator == "none" || simulator == "isaac",
            "Manager component simulator dependency is unsupported");
    components.push_back(RequiredString(row, "controllable_component_id"));
  }
  std::sort(components.begin(), components.end());
  Require(std::adjacent_find(components.begin(), components.end()) ==
              components.end(),
          "Manager component simulator dependencies repeat a component");
}

void ValidateManagerPayload(const CanonicalValue &payload,
                            const std::string &cell_id) {
  Require(payload.is_object() && payload.as_object().size() == 6U,
          "Manager source payload is not the closed v1 shape");
  Require(RequiredString(payload, "api_version") ==
              kManagerSourcePayloadApiVersion,
          "Manager source payload version is unsupported");
  Require(RequiredString(payload, "cell_id") == cell_id,
          "Manager source payload belongs to a different cell");
  const auto &active = RequiredMember(payload, "active");
  Require(active.is_object(),
          "Manager active payload is not the closed v1 shape");
  const auto *dependencies =
      active.find_object_member("component_simulator_dependencies");
  Require(active.as_object().size() == (dependencies == nullptr ? 5U : 6U),
          "Manager active payload is not the closed v1 shape");
  static_cast<void>(RequiredBool(active, "canonical_empty"));
  static_cast<void>(RequiredString(active, "graph_revision"));
  static_cast<void>(RequiredString(active, "retained_bundle_id"));
  Require(RequiredMember(active, "cell_graph").is_object(),
          "Manager active cell_graph must be an object");
  Require(RequiredMember(active, "publication").is_object(),
          "Manager active publication must be an object");
  if (dependencies != nullptr) {
    ValidateComponentSimulatorDependencies(*dependencies);
  }
  const auto manager_ready = RequiredBool(payload, "manager_ready");
  const auto &runtime = RequiredMember(payload, "runtime");
  Require(RequiredMember(payload, "apply").is_object() && runtime.is_object(),
          "Manager source payload sections are invalid");
  Require(RequiredBool(runtime, "converged") == manager_ready,
          "Manager readiness and runtime convergence disagree");
}

void ValidateScenePayload(const CanonicalValue &payload,
                          const std::string &cell_id) {
  Require(payload.is_object() && payload.as_object().size() == 3U,
          "logical-scene source payload is not the closed v1 shape");
  Require(RequiredString(payload, "api_version") ==
              kLogicalSceneSourcePayloadApiVersion,
          "logical-scene source payload version is unsupported");
  Require(RequiredMember(payload, "access").is_object(),
          "logical-scene access payload must be an object");
  const auto &snapshot = RequiredMember(payload, "snapshot");
  if (std::holds_alternative<std::nullptr_t>(snapshot.storage())) {
    return;
  }
  Require(snapshot.is_object() &&
              RequiredString(snapshot, "cell_id") == cell_id,
          "logical-scene snapshot belongs to a different cell");
}

void ValidatePoseSourcesShape(const CanonicalValue& state) {
  Require(RequiredMember(state, "pose_sources").is_array(),
          "scene/runtime pose_sources must be an array");
}

void ValidateConfiguredJointStates(const CanonicalValue &rows) {
  Require(rows.is_array(),
          "scene/runtime configured_joint_states must be an array");
  for (const auto &row : rows.as_array()) {
    Require(row.is_object() && row.as_object().size() == 4U,
            "scene/runtime configured_joint_states row is not the closed v1 "
            "shape");
    for (const std::string_view field :
         {"object_id", "policy_id", "source_id", "topic"}) {
      Require(RequiredString(row, field).size() <= 256U,
              "scene/runtime configured_joint_states identity exceeds its "
              "bound");
    }
  }
}

void ValidateSceneRuntimePayload(const CanonicalValue &payload,
                                 const std::string &cell_id) {
  Require(payload.is_object() &&
              (payload.as_object().size() == 3U ||
               payload.as_object().size() == 4U),
          "scene/runtime source payload is not the closed v1 shape");
  Require(RequiredString(payload, "api_version") ==
              kSceneRuntimeSourcePayloadApiVersion,
          "scene/runtime source payload version is unsupported");
  Require(RequiredString(payload, "cell_id") == cell_id,
          "scene/runtime source payload belongs to a different cell");
  const auto &state = RequiredMember(payload, "state");
  Require(state.is_object() && RequiredString(state, "api_version") ==
                                   kSceneRuntimeStateSnapshotApiVersion,
          "scene/runtime state payload version is unsupported");
  static_cast<void>(RequiredString(state, "state"));
  ValidatePoseSourcesShape(state);
  if (payload.as_object().size() == 4U) {
    ValidateConfiguredJointStates(
        RequiredMember(payload, "configured_joint_states"));
  }
}

// The `source_content_digest` of a section: the same function and domain the
// former status files used, so equal payloads keep their digest.
constexpr std::string_view kSourceSemanticIdentityDomain =
    "manyforge-source-snapshot-semantic";
constexpr std::string_view kSourceSemanticIdentityVersion =
    "manyforge.source_snapshot.v2";

/// The fixed owner role of each mirrored section (owners are fixed roles).
struct SectionRole final {
  std::string_view name;
  std::string_view owner_kind;
  std::string_view owner_id;
};
constexpr std::array<SectionRole, ManagerStatusMirror::kSectionCount> kRoles{{
    {"manager", "component_manager", "manager"},
    {"logical_scene", "logical_scene", "logical_scene"},
    {"scene_runtime", "scene_runtime", "scene_runtime"},
    {"viewer_pose_sessions", "scene_runtime", "scene_runtime"},
}};

std::string SemanticDigest(const SectionRole &role,
                           const CanonicalValue &payload) {
  return content_identity::framed_sha256(
      kSourceSemanticIdentityDomain, kSourceSemanticIdentityVersion,
      CanonicalValue(CanonicalValue::Object{
          {"payload", payload},
          {"source_owner_id", CanonicalValue(std::string(role.owner_id))},
          {"source_owner_kind", CanonicalValue(std::string(role.owner_kind))},
      }));
}

/// What the control state shows of a section value: pose samples, the
/// scene runtime's update time and the Manager's observation counters are
/// left out, so they do not wake the control state.
CanonicalValue ControlPayload(const CanonicalValue &value, const std::size_t index,
                              const std::string_view location = "") {
  if (!value.is_object())
    return value;
  const bool manager = index == ManagerStatusMirror::kManager;
  const bool scene_runtime = index == ManagerStatusMirror::kSceneRuntime;
  if (scene_runtime && location == "state")
    ValidatePoseSourcesShape(value);
  CanonicalValue::Object result;
  for (const auto &[key, child] : value.as_object()) {
    if (manager && location == "observed_unit" && key == "report_age_ns")
      continue;  // Observation-only information (OD-295 (1)(a)).
    if (manager && location == "observed_unit" &&
        (key == "process_observation_sequence" || key == "source_report_sequence")) {
      // Malformed ignored counters must not masquerade as valid pulses.
      if (!child.is_null())
        static_cast<void>(content_identity::parse_nonnegative_int64(child));
      continue;
    }
    if (scene_runtime && location == "state" &&
        (key == "pose_sources" || key == "updated_at_ns"))
      continue;
    if (manager && location == "runtime" && key == "observed_units" &&
        child.is_array()) {
      CanonicalValue::Array rows;
      for (const auto &row : child.as_array())
        rows.push_back(ControlPayload(row, index, "observed_unit"));
      result.emplace_back(key, CanonicalValue(std::move(rows)));
    } else if (location.empty() && ((manager && key == "runtime") ||
                                    (scene_runtime && key == "state"))) {
      result.emplace_back(key, ControlPayload(child, index, key));
    } else {
      result.emplace_back(key, child);
    }
  }
  return CanonicalValue(std::move(result));
}

void ValidateViewerPoseSessions(const CanonicalValue &payload) {
  Require(payload.is_object() && payload.as_object().size() == 2U &&
              payload.find_object_member("runtime_identity") != nullptr &&
              RequiredMember(payload, "sessions").is_array(),
          "viewer pose sessions are not the closed v1 shape");
}

[[noreturn]] void StreamInvalid(const std::string &detail) {
  throw ManagerStatusStreamError("manager_status_stream_invalid",
                                 "Manager status stream: " + detail);
}

std::uint64_t StreamCounter(const CanonicalValue &object,
                            const std::string_view name) {
  const auto *value = object.find_object_member(name);
  if (value == nullptr || !value->is_number())
    StreamInvalid(std::string(name) + " is not a counter");
  const double number = value->as_number();
  if (number < 0.0 || number > static_cast<double>(kMaximumSafeInteger) ||
      static_cast<double>(static_cast<std::uint64_t>(number)) != number)
    StreamInvalid(std::string(name) + " is not a counter");
  return static_cast<std::uint64_t>(number);
}

enum class Freshness {
  kFresh,
  kStale,
  kUnavailable,
};

std::string_view ToString(const Freshness value) noexcept {
  switch (value) {
  case Freshness::kFresh:
    return "fresh";
  case Freshness::kStale:
    return "stale";
  case Freshness::kUnavailable:
    return "unavailable";
  }
  return {};
}

struct Section final {
  std::shared_ptr<const ManagerStatusMirror::Value> source;
  Freshness freshness{Freshness::kUnavailable};
  std::optional<std::uint64_t> receiver_age_ns;
  std::vector<std::string> known_gaps;
};

CanonicalValue StringArray(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(CanonicalValue(value));
  }
  return CanonicalValue(std::move(result));
}

/// One section's freshness from the mirror. A Manager that does not answer
/// (its connection ended, or no new tick within the bound) makes every
/// section with a value stale with the one code `manager_status_unavailable`.
Section ObserveSection(const ManagerStatusMirror::View &view,
                       const std::size_t index, const bool answering,
                       const std::optional<std::uint64_t> tick_age_ns) {
  const auto &state = view.sections[index];
  const std::string name(kRoles[index].name);
  Section result;
  result.source = state.last_valid;
  if (!answering && view.connection != ManagerStatusMirror::Connection::kNeverConnected &&
      state.last_valid != nullptr) {
    result.freshness = Freshness::kStale;
    result.receiver_age_ns = tick_age_ns;
    result.known_gaps.emplace_back("manager_status_unavailable");
  } else if (state.current &&
             state.problem == ManagerStatusMirror::Problem::kOversized) {
    result.known_gaps.emplace_back("manager_status_section_oversized");
  } else if (state.current &&
             state.problem == ManagerStatusMirror::Problem::kInvalid) {
    result.known_gaps.push_back(name + "_source_invalid");
  } else if (!state.current || state.last_valid == nullptr) {
    result.known_gaps.push_back(name + "_source_absent");
  } else {
    result.freshness = Freshness::kFresh;
    result.receiver_age_ns = tick_age_ns;
  }
  return result;
}

CanonicalValue SemanticSection(const Section &section, const std::size_t index) {
  if (section.source == nullptr) {
    return CanonicalValue(CanonicalValue::Object{
        {"available", CanonicalValue(false)},
        {"freshness", CanonicalValue(std::string(ToString(section.freshness)))},
        {"known_gaps", StringArray(section.known_gaps)},
        {"payload", CanonicalValue(nullptr)},
        {"source_content_digest", CanonicalValue(nullptr)},
        {"source_generation", CanonicalValue(nullptr)},
        {"source_owner_id", CanonicalValue(nullptr)},
        {"source_owner_kind", CanonicalValue(nullptr)},
    });
  }
  const auto &source = *section.source;
  return CanonicalValue(CanonicalValue::Object{
      {"available", CanonicalValue(true)},
      {"freshness", CanonicalValue(std::string(ToString(section.freshness)))},
      {"known_gaps", StringArray(section.known_gaps)},
      {"payload", source.payload},
      {"source_content_digest", CanonicalValue(source.semantic_content_digest)},
      {"source_generation",
       Number(source.generation, "source_generation", true)},
      {"source_owner_id", CanonicalValue(std::string(kRoles[index].owner_id))},
      {"source_owner_kind",
       CanonicalValue(std::string(kRoles[index].owner_kind))},
  });
}

CanonicalValue ProjectedSection(const Section &section, const std::size_t index,
                                const std::uint64_t projection_generation) {
  CanonicalValue::Object members = SemanticSection(section, index).as_object();
  members.emplace_back(
      "projection_generation",
      Number(projection_generation, "projection_generation", true));
  members.emplace_back("receiver_age_ns",
                       section.receiver_age_ns.has_value()
                           ? Number(*section.receiver_age_ns, "receiver_age_ns")
                           : CanonicalValue(nullptr));
  members.emplace_back("source_observed_at_ns",
                       section.source != nullptr
                           ? Number(section.source->observed_at_ns,
                                    "source_observed_at_ns")
                           : CanonicalValue(nullptr));
  return CanonicalValue(std::move(members));
}

struct ActiveIdentity final {
  std::optional<std::string> retained_bundle_id;
  std::optional<std::string> graph_revision;
};

ActiveIdentity ActiveFromManager(const Section &manager) {
  if (manager.source == nullptr) {
    return {};
  }
  const auto &active = RequiredMember(manager.source->payload, "active");
  return {
      RequiredString(active, "retained_bundle_id"),
      RequiredString(active, "graph_revision"),
  };
}

CanonicalValue OptionalString(const std::optional<std::string> &value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

CanonicalValue Scope() {
  return CanonicalValue(CanonicalValue::Object{
      {"blind_spots", StringArray({
                          "program_execution_pending_i6",
                          "component_lifecycle_pending_i7",
                          "kernel_stop_pending_i8",
                          "isaac_pending_i9",
                          "real_robot_pending_i10",
                          "tools_and_grippers_pending_i11",
                      })},
      {"included_sections",
       StringArray({"manager", "logical_scene", "scene_runtime"})},
  });
}

} // namespace

ManagerStatusMirror::ManagerStatusMirror(std::string cell_id,
                                         const std::int64_t manager_silence_ns,
                                         Clock monotonic_clock_ns)
    : cell_id_(std::move(cell_id)), manager_silence_ns_(manager_silence_ns),
      monotonic_clock_ns_(std::move(monotonic_clock_ns)),
      view_(std::make_shared<const View>()) {
  Require(!cell_id_.empty(), "cell identity must be non-empty");
  Require(manager_silence_ns_ > 0 &&
              static_cast<std::uint64_t>(manager_silence_ns_) <=
                  kMaximumSafeInteger,
          "the Manager silence bound must be a positive I-JSON safe integer");
  Require(static_cast<bool>(monotonic_clock_ns_),
          "the mirror's monotonic clock is required");
}

bool ManagerStatusMirror::Answering(const View &view,
                                    const std::int64_t now_ns) const noexcept {
  return view.connection == Connection::kConnected &&
         now_ns - view.last_tick_advance_ns <= manager_silence_ns_;
}

ManagerStatusMirror::ApplyResult
ManagerStatusMirror::Apply(const std::string_view frame) {
  CanonicalValue root;
  try {
    root = content_identity::parse_canonical_json(
        frame, content_identity::CanonicalJsonParseLimits{
                   content_identity::kMaximumCanonicalJsonBytes,
                   content_identity::kMaximumCanonicalJsonDepth,
                   content_identity::kMaximumCanonicalJsonNodes});
  } catch (const std::exception &error) {
    StreamInvalid(std::string("frame is not canonical JSON: ") + error.what());
  }
  if (!root.is_object())
    StreamInvalid("frame is not an object");
  const auto *type = root.find_object_member("type");
  if (type == nullptr || !type->is_string())
    StreamInvalid("frame has no type");
  const auto &kind = type->as_string();
  if (kind == "error") {
    const auto *error = root.find_object_member("error");
    const auto *code = error == nullptr ? nullptr : error->find_object_member("code");
    const auto *message =
        error == nullptr ? nullptr : error->find_object_member("message");
    if (code == nullptr || !code->is_string() || code->as_string().empty())
      StreamInvalid("error frame has no code");
    throw ManagerStatusStreamError(
        code->as_string(), message != nullptr && message->is_string()
                               ? message->as_string()
                               : std::string("Manager status stream ended"));
  }
  const auto *api = root.find_object_member("api_version");
  if (api == nullptr || !api->is_string() ||
      api->as_string() != kManagerStatusStreamApiVersion)
    StreamInvalid("frame has another version");
  const auto *start = root.find_object_member("application_start_id");
  if (start == nullptr || !start->is_string() || start->as_string().empty())
    StreamInvalid("frame has no application start");
  const auto tick = StreamCounter(root, "tick");
  const auto now = monotonic_clock_ns_();
  const auto current = view_.load();
  auto next = std::make_shared<View>(*current);
  ApplyResult result;
  if (kind == "snapshot") {
    const auto *generations = root.find_object_member("generations");
    if (root.as_object().size() != 5U || generations == nullptr ||
        !generations->is_object() || generations->as_object().size() != kSectionCount)
      StreamInvalid("snapshot frame is not the closed v1 shape");
    for (const auto &role : kRoles)
      static_cast<void>(StreamCounter(*generations, role.name));
    // A new subscription: every section waits for its own frame.
    next->connection = Connection::kConnected;
    for (auto &section : next->sections) {
      section.current = false;
      section.problem = Problem::kNone;
    }
    // A snapshot is sent by the stream's own threads, so it proves the loop
    // ran only when its tick is above the last one seen from the same start
    // (§5.3); a new start's first snapshot counts (OD-284).
    const bool advanced = current->application_start_id != start->as_string() ||
                          tick > current->tick;
    next->application_start_id = start->as_string();
    next->tick = tick;
    if (advanced)
      next->last_tick_advance_ns = now;
    view_.store(std::move(next));
    return {true, true, advanced};
  }
  if (current->connection != Connection::kConnected)
    StreamInvalid("frame before the snapshot");
  if (tick < current->tick)
    StreamInvalid("tick moved backwards");
  if (tick > current->tick) {
    next->tick = tick;
    next->last_tick_advance_ns = now;
    result.tick_advanced = true;
  }
  if (kind == "keepalive") {
    if (root.as_object().size() != 4U)
      StreamInvalid("keepalive frame is not the closed v1 shape");
  } else if (kind == "section") {
    const auto *name = root.find_object_member("name");
    const auto *value = root.find_object_member("section");
    if (root.as_object().size() != 6U || name == nullptr || !name->is_string() ||
        value == nullptr || !value->is_object() || value->as_object().size() != 4U)
      StreamInvalid("section frame is not the closed v1 shape");
    const auto role = std::find_if(kRoles.begin(), kRoles.end(), [&](const auto &entry) {
      return entry.name == name->as_string();
    });
    if (role == kRoles.end())
      StreamInvalid("section frame names an unknown section");
    const auto index = static_cast<std::size_t>(role - kRoles.begin());
    const auto generation = StreamCounter(*value, "generation");
    const auto *observed = value->find_object_member("observed_at_ns");
    const auto *oversized = value->find_object_member("oversized");
    const auto *payload = value->find_object_member("payload");
    std::int64_t observed_at_ns{};
    try {
      if (observed == nullptr)
        throw std::invalid_argument("missing");
      observed_at_ns = content_identity::parse_nonnegative_int64(*observed);
    } catch (const std::exception &) {
      StreamInvalid("section observed_at_ns is invalid");
    }
    if (generation == 0U || oversized == nullptr || !oversized->is_bool() ||
        payload == nullptr || oversized->as_bool() != payload->is_null())
      StreamInvalid("section value is not the closed v1 shape");
    auto &section = next->sections[index];
    const auto before = section;
    section.current = true;
    if (oversized->as_bool()) {
      section.problem = Problem::kOversized;
    } else {
      try {
        switch (index) {
        case kManager:
          ValidateManagerPayload(*payload, cell_id_);
          break;
        case kLogicalScene:
          ValidateScenePayload(*payload, cell_id_);
          break;
        case kSceneRuntime:
          ValidateSceneRuntimePayload(*payload, cell_id_);
          break;
        default:
          ValidateViewerPoseSessions(*payload);
          break;
        }
        auto stored = std::make_shared<Value>();
        stored->generation = generation;
        stored->observed_at_ns = observed_at_ns;
        stored->payload = *payload;
        stored->semantic_content_digest = SemanticDigest(*role, *payload);
        if (index != kViewerPoseSessions)
          stored->control_digest = content_identity::sha256_digest(
              content_identity::canonical_json(ControlPayload(*payload, index)));
        if (index == kSceneRuntime || index == kViewerPoseSessions)
          stored->payload_json = content_identity::canonical_json(*payload);
        section.last_valid = std::move(stored);
        section.problem = Problem::kNone;
      } catch (const std::exception &) {
        // A payload the Headless cannot use: the section shows it, the
        // subscription goes on (the last valid value stays).
        section.problem = Problem::kInvalid;
      }
    }
    result.control_changed =
        index != kViewerPoseSessions &&
        (before.current != section.current || before.problem != section.problem ||
         (before.last_valid == nullptr) != (section.last_valid == nullptr) ||
         (section.last_valid != nullptr &&
          before.last_valid->control_digest != section.last_valid->control_digest));
    result.viewer_changed = index == kSceneRuntime || index == kViewerPoseSessions;
  } else {
    StreamInvalid("frame type is unknown");
  }
  view_.store(std::move(next));
  return result;
}

bool ManagerStatusMirror::MarkDisconnected() {
  const auto current = view_.load();
  if (current->connection != Connection::kConnected)
    return false;
  auto next = std::make_shared<View>(*current);
  next->connection = Connection::kDisconnected;
  view_.store(std::move(next));
  return true;
}

std::optional<std::pair<std::string, std::string>>
ManagerStatusMirror::ViewerPoseSources() const {
  const auto view = view_.load();
  if (!Answering(*view, monotonic_clock_ns_()))
    return std::nullopt;
  const auto &scene = view->sections[kSceneRuntime];
  const auto &sessions = view->sections[kViewerPoseSessions];
  if (!scene.current || scene.problem != Problem::kNone ||
      scene.last_valid == nullptr || !sessions.current ||
      sessions.problem != Problem::kNone || sessions.last_valid == nullptr)
    return std::nullopt;
  return std::pair{scene.last_valid->payload_json,
                   sessions.last_valid->payload_json};
}

std::optional<std::string>
ManagerStatusMirror::LogicalSceneActiveProjection() const {
  const auto view = view_.load();
  const auto &value = view->sections[kLogicalScene].last_valid;
  if (value == nullptr)
    return std::nullopt;
  const auto *shown =
      value->payload.find_object_member("access")->find_object_member(
          "active_projection");
  if (shown == nullptr || shown->is_null())
    return std::nullopt;
  return content_identity::canonical_json(*shown);
}

RuntimeSnapshotAggregator::RuntimeSnapshotAggregator(
    const ManagerStatusMirror &mirror, std::string projection_owner_id,
    std::string application_start_id, Clock projection_clock_ns)
    : mirror_(mirror), projection_owner_id_(std::move(projection_owner_id)),
      application_start_id_(std::move(application_start_id)),
      projection_clock_ns_(std::move(projection_clock_ns)) {
  Require(!projection_owner_id_.empty() && !application_start_id_.empty(),
          "projection-owner and start identities must be non-empty");
  Require(static_cast<bool>(projection_clock_ns_),
          "the projection clock is required");
}

HeadlessRuntimeSnapshot RuntimeSnapshotAggregator::Evaluate() {
  std::lock_guard lock(mutex_);
  const auto projected_at_ns = projection_clock_ns_();
  Require(projected_at_ns >= 0, "projection clock must be nonnegative");
  const auto status = mirror_.Load();
  const auto now_ns = mirror_.Now();
  const bool answering = mirror_.Answering(*status, now_ns);
  std::optional<std::uint64_t> tick_age_ns;
  if (status->connection != ManagerStatusMirror::Connection::kNeverConnected)
    tick_age_ns = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, now_ns - status->last_tick_advance_ns));

  const auto manager = ObserveSection(*status, ManagerStatusMirror::kManager,
                                      answering, tick_age_ns);
  const auto logical_scene = ObserveSection(
      *status, ManagerStatusMirror::kLogicalScene, answering, tick_age_ns);
  const auto scene_runtime = ObserveSection(
      *status, ManagerStatusMirror::kSceneRuntime, answering, tick_age_ns);
  const bool complete = manager.freshness == Freshness::kFresh &&
                        logical_scene.freshness == Freshness::kFresh &&
                        scene_runtime.freshness == Freshness::kFresh;
  std::vector<std::string> unavailable_sections;
  std::vector<std::string> reasons;
  const auto add_reasons = [&](const Section &section, const char *name) {
    if (section.freshness == Freshness::kFresh)
      return;
    unavailable_sections.emplace_back(name);
    for (const auto &gap : section.known_gaps) {
      // "Not answering" is one condition: it appears once.
      if (std::find(reasons.begin(), reasons.end(), gap) == reasons.end())
        reasons.push_back(gap);
    }
  };
  add_reasons(manager, "manager");
  add_reasons(logical_scene, "logical_scene");
  add_reasons(scene_runtime, "scene_runtime");
  const auto active = ActiveFromManager(manager);
  const CanonicalValue semantic(CanonicalValue::Object{
      {"active_graph_revision", OptionalString(active.graph_revision)},
      {"active_retained_bundle_id", OptionalString(active.retained_bundle_id)},
      {"application_start_id", CanonicalValue(application_start_id_)},
      {"complete", CanonicalValue(complete)},
      {"projection_owner_id", CanonicalValue(projection_owner_id_)},
      {"reasons", StringArray(reasons)},
      {"schema_version",
       CanonicalValue(std::string(kHeadlessRuntimeSnapshotSchemaVersion))},
      {"scope", Scope()},
      {"sections",
       CanonicalValue(CanonicalValue::Object{
           {"logical_scene",
            SemanticSection(logical_scene, ManagerStatusMirror::kLogicalScene)},
           {"manager", SemanticSection(manager, ManagerStatusMirror::kManager)},
           {"scene_runtime",
            SemanticSection(scene_runtime, ManagerStatusMirror::kSceneRuntime)},
       })},
      {"unavailable_sections", StringArray(unavailable_sections)},
  });
  const auto semantic_digest = content_identity::framed_sha256(
      kHeadlessRuntimeSnapshotIdentityDomain,
      kHeadlessRuntimeSnapshotSchemaVersion, semantic);
  if (projection_generation_ == 0U) {
    projection_generation_ = 1U;
  } else if (semantic_digest != previous_semantic_digest_) {
    Require(projection_generation_ < kMaximumSafeInteger,
            "projection generation is exhausted");
    ++projection_generation_;
  }
  previous_semantic_digest_ = semantic_digest;

  const CanonicalValue root(CanonicalValue::Object{
      {"active_graph_revision", OptionalString(active.graph_revision)},
      {"active_retained_bundle_id", OptionalString(active.retained_bundle_id)},
      {"application_start_id", CanonicalValue(application_start_id_)},
      {"complete", CanonicalValue(complete)},
      {"projected_at_ns", Number(projected_at_ns, "projected_at_ns")},
      {"projection_content_digest", CanonicalValue(semantic_digest)},
      {"projection_generation",
       Number(projection_generation_, "projection_generation", true)},
      {"projection_owner_id", CanonicalValue(projection_owner_id_)},
      {"reasons", StringArray(reasons)},
      {"schema_version",
       CanonicalValue(std::string(kHeadlessRuntimeSnapshotSchemaVersion))},
      {"scope", Scope()},
      {"sections",
       CanonicalValue(CanonicalValue::Object{
           {"logical_scene",
            ProjectedSection(logical_scene, ManagerStatusMirror::kLogicalScene,
                             projection_generation_)},
           {"manager", ProjectedSection(manager, ManagerStatusMirror::kManager,
                                        projection_generation_)},
           {"scene_runtime",
            ProjectedSection(scene_runtime, ManagerStatusMirror::kSceneRuntime,
                             projection_generation_)},
       })},
      {"unavailable_sections", StringArray(unavailable_sections)},
  });
  return {
      projection_generation_,
      projected_at_ns,
      semantic_digest,
      root,
  };
}

} // namespace manyforge::headless
