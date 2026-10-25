#include "manyforge_headless/runtime_snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
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
  Require(active.is_object() && active.as_object().size() == 5U,
          "Manager active payload is not the closed v1 shape");
  static_cast<void>(RequiredBool(active, "canonical_empty"));
  static_cast<void>(RequiredString(active, "graph_revision"));
  static_cast<void>(RequiredString(active, "retained_bundle_id"));
  Require(RequiredMember(active, "cell_graph").is_object(),
          "Manager active cell_graph must be an object");
  Require(RequiredMember(active, "publication").is_object(),
          "Manager active publication must be an object");
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

void ValidateCellObserver(const CanonicalValue &observer,
                          const std::string &cell_id) {
  Require(observer.is_object(),
          "scene/runtime cell_observer must be an object");
  const auto *expected_joint_state =
      observer.find_object_member("expected_joint_state");
  Require(observer.as_object().size() ==
              (expected_joint_state == nullptr ? 9U : 10U),
          "scene/runtime cell_observer is not the closed v1 shape");
  Require(RequiredString(observer, "cell_id") == cell_id,
          "scene/runtime cell_observer belongs to a different cell");
  const auto source_id = RequiredString(observer, "source_id");
  const auto source_epoch_base = RequiredString(observer, "source_epoch_base");
  constexpr std::string_view kMaximumSourceEpochSuffix = ".9007199254740991";
  Require(source_id.size() <= 256U &&
              source_epoch_base.size() <=
                  256U - kMaximumSourceEpochSuffix.size() &&
              source_epoch_base.find('\0') == std::string::npos,
          "scene/runtime cell_observer source identity is invalid");
  const auto availability = RequiredString(observer, "availability");
  Require(availability == "available" || availability == "pending" ||
              availability == "unavailable" || availability == "unknown",
          "scene/runtime cell_observer availability is unsupported");
  static_cast<void>(RequiredBool(observer, "observation_current"));
  const auto presence = RequiredString(observer, "process_presence");
  Require(presence == "present" || presence == "absent" ||
              presence == "unknown",
          "scene/runtime cell_observer process_presence is unsupported");
  if (expected_joint_state != nullptr &&
      !std::holds_alternative<std::nullptr_t>(
          expected_joint_state->storage())) {
    Require(expected_joint_state->is_object() &&
                expected_joint_state->as_object().size() == 3U,
            "scene/runtime expected_joint_state is not the closed v1 shape");
    const auto object_id = RequiredString(*expected_joint_state, "object_id");
    const auto policy_id = RequiredString(*expected_joint_state, "policy_id");
    const auto topic = RequiredString(*expected_joint_state, "topic");
    Require(object_id.size() <= 256U && policy_id.size() <= 256U &&
                topic.size() <= 256U,
            "scene/runtime expected_joint_state identity exceeds its bound");
  }
  const auto &exit = RequiredMember(observer, "exit");
  Require(std::holds_alternative<std::nullptr_t>(exit.storage()) ||
              exit.is_object(),
          "scene/runtime cell_observer exit must be null or an object");
  if (exit.is_object()) {
    Require(exit.as_object().size() == 6U,
            "scene/runtime cell_observer exit is not the closed v1 shape");
    const auto exact_integer = [](const CanonicalValue &value,
                                  const double minimum,
                                  const double maximum) {
      const auto *number = std::get_if<double>(&value.storage());
      return number != nullptr && std::isfinite(*number) &&
             *number >= minimum && *number <= maximum &&
             std::floor(*number) == *number;
    };
    Require(exact_integer(RequiredMember(exit, "pid"), 1.0, 2147483647.0),
            "scene/runtime cell_observer exit pid is invalid");
    static_cast<void>(content_identity::parse_nonnegative_int64(
        RequiredMember(exit, "detected_at_ns")));
    const auto nullable_byte = [&](const std::string_view field) {
      const auto &value = RequiredMember(exit, field);
      return std::holds_alternative<std::nullptr_t>(value.storage()) ||
             exact_integer(value, 0.0, 255.0);
    };
    Require(nullable_byte("exit_status") && nullable_byte("signal"),
            "scene/runtime cell_observer exit status field is invalid");
    const bool has_exit_status = !std::holds_alternative<std::nullptr_t>(
        RequiredMember(exit, "exit_status").storage());
    const bool has_signal = !std::holds_alternative<std::nullptr_t>(
        RequiredMember(exit, "signal").storage());
    Require(has_exit_status != has_signal,
            "scene/runtime cell_observer exit must select status or signal");
    static_cast<void>(RequiredBool(exit, "core_dumped"));
    const auto *observed_utc = std::get_if<std::string>(
        &RequiredMember(exit, "observed_utc").storage());
    Require(observed_utc != nullptr && observed_utc->size() <= 128U,
            "scene/runtime cell_observer exit observed_utc is invalid");
  }
  const auto &incident_id = RequiredMember(observer, "incident_id");
  const auto *incident = std::get_if<std::string>(&incident_id.storage());
  Require(std::holds_alternative<std::nullptr_t>(incident_id.storage()) ||
              (incident != nullptr && !incident->empty() &&
               incident->size() <= 128U),
          "scene/runtime cell_observer incident_id must be null or a string");
  const auto *persistence = std::get_if<double>(
      &RequiredMember(observer, "incident_persistence").storage());
  Require(persistence != nullptr && std::isfinite(*persistence) &&
              *persistence >= 0.0 && std::floor(*persistence) == *persistence &&
              *persistence <= 4.0,
          "scene/runtime cell_observer incident_persistence must be a number");
  if (std::holds_alternative<std::nullptr_t>(exit.storage())) {
    Require(incident == nullptr && *persistence == 0.0,
            "scene/runtime cell_observer non-exit incident state is invalid");
  } else if (incident == nullptr) {
    Require(*persistence == 4.0,
            "scene/runtime cell_observer unknown exit incident is invalid");
  } else {
    Require(*persistence >= 1.0,
            "scene/runtime cell_observer exit incident state is invalid");
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
    ValidateCellObserver(RequiredMember(payload, "cell_observer"), cell_id);
  }
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
  std::optional<SourceSnapshotFileObservation> source;
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

CanonicalValue ControlPayload(const CanonicalValue& value, const std::string_view kind,
                              const std::string_view location = "") {
  if (!value.is_object()) return value;
  if (kind == "scene-runtime-source.json" && location == "state") ValidatePoseSourcesShape(value);
  CanonicalValue::Object result;
  for (const auto& [key, child] : value.as_object()) {
    if (kind == "manager-source.json" && location == "observed_unit" &&
        (key == "process_observation_sequence" || key == "source_report_sequence")) {
      // Malformed ignored counters must not masquerade as valid pulses.
      if (!child.is_null()) static_cast<void>(content_identity::parse_nonnegative_int64(child));
      continue;
    }
    if (kind == "scene-runtime-source.json" && location == "state" &&
        (key == "pose_sources" || key == "updated_at_ns"))
      continue;
    if (kind == "manager-source.json" && location == "runtime" && key == "observed_units" &&
        child.is_array()) {
      CanonicalValue::Array rows;
      for (const auto& row : child.as_array())
        rows.push_back(ControlPayload(row, kind, "observed_unit"));
      result.emplace_back(key, CanonicalValue(std::move(rows)));
    } else if (location.empty() && ((kind == "manager-source.json" && key == "runtime") ||
                                    (kind == "scene-runtime-source.json" && key == "state"))) {
      result.emplace_back(key, ControlPayload(child, kind, key));
    } else {
      result.emplace_back(key, child);
    }
  }
  return CanonicalValue(std::move(result));
}

Section ObserveSection(
    AtomicSourceSnapshotChannel &channel,
    std::optional<SourceSnapshotFileObservation> &last_valid,
    const std::string_view expected_owner_kind,
    const std::string &expected_owner_id,
    const std::function<std::int64_t()> &wall_clock_ns,
    const std::int64_t maximum_age_ns,
    const std::function<void(const CanonicalValue &)> &validate_payload,
    const std::string_view section_name) {
  std::optional<SourceSnapshotFileObservation> current;
  std::string failure_code;
  try {
    current = channel.ReadObserved();
    if (!current.has_value()) {
      failure_code = std::string(section_name) + "_source_absent";
    } else if (current->snapshot.source_owner_kind != expected_owner_kind ||
               current->snapshot.source_owner_id != expected_owner_id) {
      failure_code = std::string(section_name) + "_source_identity_mismatch";
      current.reset();
    } else {
      validate_payload(current->snapshot.payload);
      last_valid = current;
    }
  } catch (...) {
    failure_code = std::string(section_name) + "_source_invalid";
    current.reset();
  }
  if (!current.has_value()) {
    Section result;
    result.source = last_valid;
    result.known_gaps.push_back(
        failure_code.empty() ? std::string(section_name) + "_source_unavailable"
                             : failure_code);
    return result;
  }

  // Sample after the atomic file observation. A source may publish between
  // an earlier clock read and open/stat/read; comparing that newer mtime to
  // the earlier sample would invent a publication-time-in-future outage.
  const auto now_wall_ns = wall_clock_ns();
  Require(now_wall_ns >= 0, "wall clock must be nonnegative");
  Section result;
  result.source = current;
  if (current->file_modified_at_unix_ns > now_wall_ns) {
    result.known_gaps.push_back(std::string(section_name) +
                                "_publication_time_in_future");
    return result;
  }
  const auto raw_age = static_cast<std::uint64_t>(
      now_wall_ns - current->file_modified_at_unix_ns);
  result.receiver_age_ns = std::min(raw_age, kMaximumSafeInteger);
  if (raw_age > kMaximumSafeInteger) {
    result.freshness = Freshness::kStale;
    result.known_gaps.push_back(std::string(section_name) +
                                "_receiver_age_saturated");
  } else if (raw_age > static_cast<std::uint64_t>(maximum_age_ns)) {
    result.freshness = Freshness::kStale;
    result.known_gaps.push_back(std::string(section_name) + "_source_stale");
  } else {
    result.freshness = Freshness::kFresh;
  }
  return result;
}

CanonicalValue SemanticSection(const Section &section) {
  if (!section.source.has_value()) {
    return CanonicalValue(CanonicalValue::Object{
        {"available", CanonicalValue(false)},
        {"freshness", CanonicalValue(std::string(ToString(section.freshness)))},
        {"known_gaps", StringArray(section.known_gaps)},
        {"payload", CanonicalValue(nullptr)},
        {"source_content_digest", CanonicalValue(nullptr)},
        {"source_generation", CanonicalValue(nullptr)},
        {"source_owner_epoch", CanonicalValue(nullptr)},
        {"source_owner_id", CanonicalValue(nullptr)},
        {"source_owner_kind", CanonicalValue(nullptr)},
    });
  }
  const auto &source = section.source->snapshot;
  return CanonicalValue(CanonicalValue::Object{
      {"available", CanonicalValue(true)},
      {"freshness", CanonicalValue(std::string(ToString(section.freshness)))},
      {"known_gaps", StringArray(section.known_gaps)},
      {"payload", source.payload},
      {"source_content_digest", CanonicalValue(source.semantic_content_digest)},
      {"source_generation",
       Number(source.source_generation, "source_generation", true)},
      {"source_owner_epoch", CanonicalValue(source.source_owner_epoch)},
      {"source_owner_id", CanonicalValue(source.source_owner_id)},
      {"source_owner_kind", CanonicalValue(source.source_owner_kind)},
  });
}

CanonicalValue ProjectedSection(const Section &section,
                                const std::uint64_t projection_generation) {
  CanonicalValue::Object members = SemanticSection(section).as_object();
  members.emplace_back(
      "projection_generation",
      Number(projection_generation, "projection_generation", true));
  members.emplace_back("receiver_age_ns",
                       section.receiver_age_ns.has_value()
                           ? Number(*section.receiver_age_ns, "receiver_age_ns")
                           : CanonicalValue(nullptr));
  members.emplace_back("source_observed_at_ns",
                       section.source.has_value()
                           ? Number(section.source->snapshot.observed_at_ns,
                                    "source_observed_at_ns")
                           : CanonicalValue(nullptr));
  return CanonicalValue(std::move(members));
}

struct ActiveIdentity final {
  std::optional<std::string> retained_bundle_id;
  std::optional<std::string> graph_revision;
};

ActiveIdentity ActiveFromManager(const Section &manager) {
  if (!manager.source.has_value()) {
    return {};
  }
  const auto &active =
      RequiredMember(manager.source->snapshot.payload, "active");
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

std::optional<std::pair<std::string, std::int64_t>> ReadControlSourceFileHint(
    const std::filesystem::path& path) {
  try {
    const auto observed =
        AtomicSourceSnapshotChannel(path.parent_path(), path.filename().string()).ReadObserved();
    if (!observed) return std::nullopt;
    const auto& source = observed->snapshot;
    const auto meaning = CanonicalValue(CanonicalValue::Object{
        {"payload", ControlPayload(source.payload, path.filename().string())},
        {"source_owner_epoch", CanonicalValue(source.source_owner_epoch)},
        {"source_owner_id", CanonicalValue(source.source_owner_id)},
        {"source_owner_kind", CanonicalValue(source.source_owner_kind)},
    });
    return std::pair{content_identity::sha256_digest(content_identity::canonical_json(meaning)),
                     observed->file_modified_at_unix_ns};
  } catch (const SourceSnapshotError&) {
    return std::nullopt;
  } catch (const RuntimeSnapshotError&) {
    return std::nullopt;
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

RuntimeSnapshotAggregator::RuntimeSnapshotAggregator(
    std::filesystem::path publication_root, std::string cell_id,
    std::string projection_owner_id, std::string projection_owner_epoch,
    const RuntimeSnapshotFreshnessPolicy freshness_policy, Clock wall_clock_ns,
    Clock projection_clock_ns)
    : manager_source_(publication_root, "manager-source.json"),
      logical_scene_source_(std::move(publication_root),
                            "logical-scene-source.json"),
      scene_runtime_source_(logical_scene_source_.directory(),
                            "scene-runtime-source.json"),
      cell_id_(std::move(cell_id)),
      projection_owner_id_(std::move(projection_owner_id)),
      projection_owner_epoch_(std::move(projection_owner_epoch)),
      freshness_policy_(freshness_policy),
      wall_clock_ns_(std::move(wall_clock_ns)),
      projection_clock_ns_(std::move(projection_clock_ns)) {
  Require(!cell_id_.empty() && !projection_owner_id_.empty() &&
              !projection_owner_epoch_.empty(),
          "cell and projection-owner identities must be non-empty");
  Require(freshness_policy_.manager_maximum_age_ns > 0 &&
              freshness_policy_.logical_scene_maximum_age_ns > 0 &&
              static_cast<std::uint64_t>(
                  freshness_policy_.manager_maximum_age_ns) <=
                  kMaximumSafeInteger &&
              static_cast<std::uint64_t>(
                  freshness_policy_.logical_scene_maximum_age_ns) <=
                  kMaximumSafeInteger &&
              freshness_policy_.scene_runtime_maximum_age_ns > 0 &&
              static_cast<std::uint64_t>(
                  freshness_policy_.scene_runtime_maximum_age_ns) <=
                  kMaximumSafeInteger,
          "freshness budgets must be positive I-JSON safe integers");
  Require(static_cast<bool>(wall_clock_ns_) &&
              static_cast<bool>(projection_clock_ns_),
          "both projection clocks are required");
}

HeadlessRuntimeSnapshot RuntimeSnapshotAggregator::Evaluate() {
  std::lock_guard lock(mutex_);
  const auto initial_wall_ns = wall_clock_ns_();
  const auto projected_at_ns = projection_clock_ns_();
  Require(initial_wall_ns >= 0 && projected_at_ns >= 0,
          "projection clocks must be nonnegative");

  const auto manager = ObserveSection(
      manager_source_, last_manager_source_, "component_manager",
      "component-manager." + cell_id_, wall_clock_ns_,
      freshness_policy_.manager_maximum_age_ns,
      [this](const CanonicalValue &payload) {
        ValidateManagerPayload(payload, cell_id_);
      },
      "manager");
  const auto logical_scene = ObserveSection(
      logical_scene_source_, last_logical_scene_source_, "logical_scene",
      "logical-scene." + cell_id_, wall_clock_ns_,
      freshness_policy_.logical_scene_maximum_age_ns,
      [this](const CanonicalValue &payload) {
        ValidateScenePayload(payload, cell_id_);
      },
      "logical_scene");
  const auto scene_runtime = ObserveSection(
      scene_runtime_source_, last_scene_runtime_source_, "scene_runtime",
      "scene-runtime." + cell_id_, wall_clock_ns_,
      freshness_policy_.scene_runtime_maximum_age_ns,
      [this](const CanonicalValue &payload) {
        ValidateSceneRuntimePayload(payload, cell_id_);
      },
      "scene_runtime");
  const bool complete = manager.freshness == Freshness::kFresh &&
                        logical_scene.freshness == Freshness::kFresh &&
                        scene_runtime.freshness == Freshness::kFresh;
  std::vector<std::string> unavailable_sections;
  std::vector<std::string> reasons;
  if (manager.freshness != Freshness::kFresh) {
    unavailable_sections.emplace_back("manager");
    reasons.insert(reasons.end(), manager.known_gaps.begin(),
                   manager.known_gaps.end());
  }
  if (logical_scene.freshness != Freshness::kFresh) {
    unavailable_sections.emplace_back("logical_scene");
    reasons.insert(reasons.end(), logical_scene.known_gaps.begin(),
                   logical_scene.known_gaps.end());
  }
  if (scene_runtime.freshness != Freshness::kFresh) {
    unavailable_sections.emplace_back("scene_runtime");
    reasons.insert(reasons.end(), scene_runtime.known_gaps.begin(),
                   scene_runtime.known_gaps.end());
  }
  const auto active = ActiveFromManager(manager);
  const CanonicalValue semantic(CanonicalValue::Object{
      {"active_graph_revision", OptionalString(active.graph_revision)},
      {"active_retained_bundle_id", OptionalString(active.retained_bundle_id)},
      {"complete", CanonicalValue(complete)},
      {"projection_owner_epoch", CanonicalValue(projection_owner_epoch_)},
      {"projection_owner_id", CanonicalValue(projection_owner_id_)},
      {"reasons", StringArray(reasons)},
      {"schema_version",
       CanonicalValue(std::string(kHeadlessRuntimeSnapshotSchemaVersion))},
      {"scope", Scope()},
      {"sections", CanonicalValue(CanonicalValue::Object{
                       {"logical_scene", SemanticSection(logical_scene)},
                       {"manager", SemanticSection(manager)},
                       {"scene_runtime", SemanticSection(scene_runtime)},
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
      {"complete", CanonicalValue(complete)},
      {"projected_at_ns", Number(projected_at_ns, "projected_at_ns")},
      {"projection_content_digest", CanonicalValue(semantic_digest)},
      {"projection_generation",
       Number(projection_generation_, "projection_generation", true)},
      {"projection_owner_epoch", CanonicalValue(projection_owner_epoch_)},
      {"projection_owner_id", CanonicalValue(projection_owner_id_)},
      {"reasons", StringArray(reasons)},
      {"schema_version",
       CanonicalValue(std::string(kHeadlessRuntimeSnapshotSchemaVersion))},
      {"scope", Scope()},
      {"sections",
       CanonicalValue(CanonicalValue::Object{
           {"logical_scene",
            ProjectedSection(logical_scene, projection_generation_)},
           {"manager", ProjectedSection(manager, projection_generation_)},
           {"scene_runtime",
            ProjectedSection(scene_runtime, projection_generation_)},
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
