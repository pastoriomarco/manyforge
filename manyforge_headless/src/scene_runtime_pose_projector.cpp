#include "manyforge_headless/scene_runtime_pose_projector.hpp"

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;

using content_identity::CanonicalValue;

constexpr std::string_view kPoseObservationPayloadApiVersion =
    "manyforge.ros_pose_observation.v2";
constexpr std::string_view kPoseApplicationRequestApiVersion =
    "manyforge.scene_runtime_pose_application.v1";
constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumTextBytes = 1024U;
constexpr std::string_view kProviderFreshnessUnsupported =
    "provider_freshness_unsupported";

core::ErrorDTO Error(const std::string_view code,
                     const std::string_view message,
                     std::map<std::string, std::string> details = {}) {
  return {std::string(code), std::string(message), std::move(details)};
}

const CanonicalValue &Member(const CanonicalValue &value,
                             const std::string_view name) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::invalid_argument("pose payload is missing field " +
                                std::string(name));
  }
  return *member;
}

void ExactObject(const CanonicalValue &value, const std::size_t size,
                 const std::string_view label) {
  if (!value.is_object() || value.as_object().size() != size) {
    throw std::invalid_argument(std::string(label) +
                                " has an unknown or missing field");
  }
}

const std::string &Text(const CanonicalValue &value,
                        const std::string_view label,
                        const bool allow_empty = false) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr || (!allow_empty && text->empty()) ||
      text->size() > kMaximumTextBytes ||
      !content_identity::is_utf8_nfc(*text)) {
    throw std::invalid_argument(std::string(label) +
                                " must be bounded NFC text");
  }
  return *text;
}

double Number(const CanonicalValue &value, const std::string_view label) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    throw std::invalid_argument(std::string(label) + " must be finite");
  }
  return *number;
}

const CanonicalValue::Array &Array(const CanonicalValue &value,
                                   const std::size_t size,
                                   const std::string_view label) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr || array->size() != size) {
    throw std::invalid_argument(std::string(label) +
                                " has the wrong array shape");
  }
  return *array;
}

std::optional<std::int64_t> SourceTimestampNs(const CanonicalValue &value) {
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return std::nullopt;
  }
  return content_identity::parse_nonnegative_int64(value);
}

void ValidateSource(const std::string_view kind, const CanonicalValue &source) {
  if (kind == "simulation_entity_pose") {
    ExactObject(source, 1U, "simulation pose source");
    static_cast<void>(Text(Member(source, "entity"), "simulation entity"));
    return;
  }
  if (kind != "detection3d_pose") {
    throw std::invalid_argument("pose payload kind is unsupported");
  }
  ExactObject(source, 5U, "Detection3D pose source");
  static_cast<void>(
      Text(Member(source, "class_id"), "detection class_id", true));
  const auto confidence =
      Number(Member(source, "confidence"), "detection confidence");
  static_cast<void>(Text(Member(source, "detection_id"), "detection id", true));
  const auto variance = Number(Member(source, "max_position_variance_m2"),
                               "detection max_position_variance_m2");
  static_cast<void>(Text(Member(source, "topic"), "detection topic"));
  if (confidence < 0.0 || confidence > 1.0 || variance < 0.0) {
    throw std::invalid_argument(
        "detection confidence or variance is outside its closed range");
  }
}

struct DecodedPose final {
  core::Pose3DTO pose;
  std::optional<std::int64_t> source_timestamp_ns;
};

DecodedPose DecodePose(const CanonicalValue &payload) {
  ExactObject(payload, 4U, "pose observation payload");
  if (Text(Member(payload, "api_version"), "pose payload api_version") !=
      kPoseObservationPayloadApiVersion) {
    throw std::invalid_argument("pose payload api_version is unsupported");
  }
  const auto &kind = Text(Member(payload, "kind"), "pose payload kind");
  ValidateSource(kind, Member(payload, "source"));
  const auto &pose = Member(payload, "pose");
  ExactObject(pose, 5U, "pose payload pose");
  if (Text(Member(pose, "frame_kind"), "pose frame_kind") != "cell_frame") {
    throw std::invalid_argument(
        "pose payload is not in the semantic cell frame");
  }
  static_cast<void>(Text(Member(pose, "frame_id"), "pose frame_id"));
  const auto &position =
      Array(Member(pose, "position_m"), 3U, "pose position_m");
  const auto &orientation =
      Array(Member(pose, "orientation_xyzw"), 4U, "pose orientation_xyzw");
  core::Pose3DTO result{
      {Number(position[0], "pose position x"),
       Number(position[1], "pose position y"),
       Number(position[2], "pose position z")},
      {Number(orientation[0], "pose orientation x"),
       Number(orientation[1], "pose orientation y"),
       Number(orientation[2], "pose orientation z"),
       Number(orientation[3], "pose orientation w")},
  };
  const auto norm_squared = result.orientation.x * result.orientation.x +
                            result.orientation.y * result.orientation.y +
                            result.orientation.z * result.orientation.z +
                            result.orientation.w * result.orientation.w;
  if (!std::isfinite(norm_squared) || norm_squared <= 0.0 ||
      std::abs(norm_squared - 1.0) > 1.0e-6) {
    throw std::invalid_argument("pose orientation is not one unit quaternion");
  }
  const auto inverse_norm = 1.0 / std::sqrt(norm_squared);
  result.orientation.x *= inverse_norm;
  result.orientation.y *= inverse_norm;
  result.orientation.z *= inverse_norm;
  result.orientation.w *= inverse_norm;
  return {result, SourceTimestampNs(Member(pose, "source_stamp_ns"))};
}

const core::CapabilityProviderBindingDTO *SelectProvider(
    const std::span<const core::CapabilityProviderBindingDTO> bindings,
    const core::CellId &cell_id) {
  const core::CapabilityProviderBindingDTO *selected = nullptr;
  for (const auto &binding : bindings) {
    if (binding.capability_id.value() != "scene.edit.update_pose" ||
        binding.scope != core::CapabilityScope::kCellGlobal) {
      continue;
    }
    const auto *target =
        std::get_if<core::CellGlobalCapabilityProviderTargetDTO>(
            &binding.target);
    if (target == nullptr || target->cell_id != cell_id) {
      continue;
    }
    if (selected != nullptr) {
      throw std::invalid_argument(
          "active bundle has more than one cell-global pose-update provider");
    }
    selected = &binding;
  }
  if (selected == nullptr) {
    throw std::invalid_argument(
        "active bundle has no exact cell-global pose-update provider");
  }
  return selected;
}

CanonicalValue Unsigned(const std::uint64_t value,
                        const std::string_view label) {
  if (value > kMaximumSafeInteger) {
    throw std::invalid_argument(std::string(label) +
                                " exceeds the I-JSON safe range");
  }
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Integer(const std::int64_t value, const std::string_view label) {
  if (value < 0) {
    throw std::invalid_argument(std::string(label) + " must be nonnegative");
  }
  return content_identity::nonnegative_int64_value(value);
}

CanonicalValue
ProjectionRevisionValue(const core::ProjectionRevisionDTO &revision) {
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(revision.graph_revision.value())},
      {"retained_bundle_id",
       CanonicalValue(revision.retained_bundle_id.value())},
      {"scene_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(revision.scene_revision.content_hash.value())},
           {"revision_id",
            Unsigned(revision.scene_revision.revision_id, "scene revision")},
           {"scene_id",
            CanonicalValue(revision.scene_revision.scene_id.value())},
       })},
  });
}

std::string SceneRequestId(const core::ProjectionRevisionDTO &projection,
                           const core::SceneObjectProviderEvidenceDTO &evidence,
                           const PoseSourceObservation &observation) {
  return "scene-runtime-pose-" +
         content_identity::sha256_digest(content_identity::canonical_json(
             CanonicalValue(CanonicalValue::Object{
                 {"api_version", CanonicalValue(std::string(
                                     kPoseApplicationRequestApiVersion))},
                 {"expected_projection_revision",
                  ProjectionRevisionValue(projection)},
                 {"expires_at_ns",
                  Integer(observation.expires_at_ns, "expires_at_ns")},
                 {"object_id", CanonicalValue(observation.key.object_id)},
                 {"payload", observation.payload},
                 {"policy_id", CanonicalValue(observation.key.policy_id)},
                 {"provider_source_epoch",
                  CanonicalValue(evidence.provider_source_epoch)},
                 {"received_at_ns",
                  Integer(evidence.received_at_ns, "received_at_ns")},
                 {"source_epoch", CanonicalValue(observation.source_epoch)},
                 {"source_epoch_order", Unsigned(observation.source_epoch_order,
                                                 "source_epoch_order")},
                 {"source_id", CanonicalValue(observation.key.source_id)},
                 {"source_sequence",
                  Unsigned(observation.source_sequence, "source_sequence")},
             })));
}

bool SameProviderOrigin(const core::SceneObjectProviderEvidenceDTO &left,
                        const core::SceneObjectProviderEvidenceDTO &right) {
  return left.provider_instance_id == right.provider_instance_id &&
         left.provider_manifest_id == right.provider_manifest_id &&
         left.provider_manifest_revision == right.provider_manifest_revision &&
         left.provider_source_epoch == right.provider_source_epoch;
}

} // namespace

std::string
SceneRuntimePoseProviderSourceEpoch(const PoseSourceObservation &observation) {
  return content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(kPoseApplicationRequestApiVersion))},
          {"source_epoch", CanonicalValue(observation.source_epoch)},
          {"source_id", CanonicalValue(observation.key.source_id)},
      })));
}

std::string_view
ToString(const SceneRuntimePoseApplicationStatus status) noexcept {
  switch (status) {
  case SceneRuntimePoseApplicationStatus::kNotRequested:
    return "not_requested";
  case SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning:
    return "deferred_before_planning";
  case SceneRuntimePoseApplicationStatus::kBuffered:
    return "buffered";
  case SceneRuntimePoseApplicationStatus::kCommitted:
    return "committed";
  case SceneRuntimePoseApplicationStatus::kAlreadyApplied:
    return "already_applied";
  case SceneRuntimePoseApplicationStatus::kSkippedAttached:
    return "skipped_attached";
  case SceneRuntimePoseApplicationStatus::kFreshnessConfirmed:
    return "freshness_confirmed";
  }
  return {};
}

bool SceneRuntimePoseApplicationRetainsPlanningEvidence(
    const SceneRuntimePoseApplicationStatus status,
    const std::uint64_t retained_provider_source_sequence,
    const std::uint64_t observation_source_sequence) noexcept {
  switch (status) {
  case SceneRuntimePoseApplicationStatus::kCommitted:
  case SceneRuntimePoseApplicationStatus::kAlreadyApplied:
    return retained_provider_source_sequence == observation_source_sequence;
  case SceneRuntimePoseApplicationStatus::kFreshnessConfirmed:
    return retained_provider_source_sequence < observation_source_sequence;
  case SceneRuntimePoseApplicationStatus::kNotRequested:
  case SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning:
  case SceneRuntimePoseApplicationStatus::kBuffered:
  case SceneRuntimePoseApplicationStatus::kSkippedAttached:
    return false;
  }
  return false;
}

bool SceneRuntimePoseApplicationResult::valid() const noexcept {
  switch (status) {
  case SceneRuntimePoseApplicationStatus::kNotRequested:
  case SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning:
  case SceneRuntimePoseApplicationStatus::kBuffered:
    return !scene_request_id.has_value() && !projection_revision.has_value() &&
           !update_status.has_value();
  case SceneRuntimePoseApplicationStatus::kCommitted:
    return scene_request_id.has_value() && !scene_request_id->empty() &&
           projection_revision.has_value() && projection_revision->valid() &&
           update_status.has_value();
  case SceneRuntimePoseApplicationStatus::kAlreadyApplied:
  case SceneRuntimePoseApplicationStatus::kSkippedAttached:
  case SceneRuntimePoseApplicationStatus::kFreshnessConfirmed:
    return scene_request_id.has_value() && !scene_request_id->empty() &&
           projection_revision.has_value() && projection_revision->valid() &&
           !update_status.has_value();
  }
  return false;
}

SceneRuntimePoseProjector::SceneRuntimePoseProjector(
    core::ISceneAuthority &scene_authority, core::CellId cell_id)
    : scene_authority_(scene_authority), cell_id_(std::move(cell_id)) {
  if (cell_id_.empty()) {
    throw std::invalid_argument("pose projector cell_id must be non-empty");
  }
}

core::Result<SceneRuntimePoseApplicationResult>
SceneRuntimePoseProjector::Apply(
    const std::span<const core::CapabilityProviderBindingDTO> provider_bindings,
    const core::RetainedBundleId &active_retained_bundle_id,
    const core::GraphRevision &active_graph_revision,
    const PoseSourceObservation &observation,
    const std::optional<SceneRuntimePoseApplicationResult> &prior_application) {
  if (!observation.valid() || observation.source_epoch_order == 0U ||
      observation.source_sequence == 0U ||
      observation.payload_quality != PoseSourceQuality::kGood ||
      !observation.eligible ||
      (observation.key.policy_id != "continuous_unattached" &&
       observation.key.policy_id != "before_planning")) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(Error(
        "scene_runtime_pose_observation_ineligible",
        "pose projection requires one good eligible admitted observation"));
  }

  const core::CapabilityProviderBindingDTO *provider = nullptr;
  DecodedPose decoded;
  try {
    provider = SelectProvider(provider_bindings, cell_id_);
  } catch (const std::invalid_argument &error) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_provider_invalid", error.what()));
  }
  try {
    decoded = DecodePose(observation.payload);
  } catch (const std::invalid_argument &error) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_payload_invalid", error.what()));
  }

  core::ProjectionRevisionDTO projection;
  try {
    projection = scene_authority_.CurrentProjectionRevision();
  } catch (const std::exception &error) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_scene_unavailable", error.what()));
  }
  if (!projection.valid() ||
      projection.retained_bundle_id != active_retained_bundle_id ||
      projection.graph_revision != active_graph_revision) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_projection_changed",
              "logical-scene projection does not match the admitted runtime"));
  }

  const auto freshness_budget =
      observation.expires_at_ns - observation.observed_at_ns;
  core::SceneObjectProviderEvidenceDTO evidence{
      provider->provider_instance_id,
      provider->provider_manifest_id,
      provider->provider_manifest_revision,
      SceneRuntimePoseProviderSourceEpoch(observation),
      observation.source_sequence,
      observation.observed_at_ns,
      decoded.source_timestamp_ns,
      freshness_budget,
  };
  if (!evidence.valid()) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_evidence_invalid",
              "derived pose-provider evidence is invalid"));
  }

  const auto request_id =
      core::RequestId(SceneRequestId(projection, evidence, observation));
  core::SceneObjectStateDTO object_state{
      core::SceneResourceId(observation.key.object_id), false, false,
      std::nullopt, std::nullopt};
  try {
    object_state = scene_authority_.GetObjectState(
        core::SceneResourceId(observation.key.object_id));
  } catch (const std::exception &error) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_scene_unavailable", error.what()));
  }
  if (!object_state.exists) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_object_missing",
              "configured pose-source object is absent from the logical scene",
              {{"scene_resource_id", observation.key.object_id}}));
  }
  if (object_state.attached) {
    auto result = SceneRuntimePoseApplicationResult{
        SceneRuntimePoseApplicationStatus::kSkippedAttached, request_id,
        projection, std::nullopt};
    return core::Result<SceneRuntimePoseApplicationResult>::Ok(
        std::move(result));
  }

  if (object_state.provider_evidence.has_value() &&
      SameProviderOrigin(*object_state.provider_evidence, evidence)) {
    const auto &previous = *object_state.provider_evidence;
    if (previous.provider_source_sequence > evidence.provider_source_sequence) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          Error("scene_runtime_pose_observation_stale",
                "logical scene already contains a newer provider observation",
                {{"previous_source_sequence",
                  std::to_string(previous.provider_source_sequence)},
                 {"received_source_sequence",
                  std::to_string(evidence.provider_source_sequence)}}));
    }
    if (previous.provider_source_sequence ==
        evidence.provider_source_sequence) {
      if (previous.source_timestamp_ns != evidence.source_timestamp_ns ||
          previous.source_freshness_budget_ns !=
              evidence.source_freshness_budget_ns) {
        return core::Result<SceneRuntimePoseApplicationResult>::Err(
            Error("scene_runtime_pose_evidence_conflict",
                  "provider observation identity has changed timing evidence"));
      }
      core::Result<core::Pose3DTO> current_pose;
      try {
        current_pose = scene_authority_.GetObjectPose(
            core::SceneResourceId(observation.key.object_id));
      } catch (const std::exception &error) {
        return core::Result<SceneRuntimePoseApplicationResult>::Err(
            Error("scene_runtime_pose_scene_unavailable", error.what()));
      }
      if (!current_pose.ok()) {
        return core::Result<SceneRuntimePoseApplicationResult>::Err(
            Error("scene_runtime_pose_scene_unavailable",
                  current_pose.error().message.empty()
                      ? "logical-scene object pose is unavailable"
                      : current_pose.error().message,
                  {{"scene_error_code", current_pose.error().code}}));
      }
      if (current_pose.value() != decoded.pose) {
        return core::Result<SceneRuntimePoseApplicationResult>::Err(
            Error("scene_runtime_pose_evidence_conflict",
                  "provider observation identity has changed pose content"));
      }
      auto result = SceneRuntimePoseApplicationResult{
          SceneRuntimePoseApplicationStatus::kAlreadyApplied, request_id,
          projection, std::nullopt};
      return core::Result<SceneRuntimePoseApplicationResult>::Ok(
          std::move(result));
    }
    // A strictly newer observation of the exact committed pose with the same
    // origin and freshness budget only refreshes runtime freshness. The
    // source timestamp is diagnostic and never gates this branch.
    if (previous.source_freshness_budget_ns ==
            evidence.source_freshness_budget_ns &&
        previous.received_at_ns <= evidence.received_at_ns) {
      core::Result<core::Pose3DTO> current_pose;
      try {
        current_pose = scene_authority_.GetObjectPose(
            core::SceneResourceId(observation.key.object_id));
      } catch (const std::exception &error) {
        return core::Result<SceneRuntimePoseApplicationResult>::Err(
            Error("scene_runtime_pose_scene_unavailable", error.what()));
      }
      if (!current_pose.ok()) {
        return core::Result<SceneRuntimePoseApplicationResult>::Err(
            Error("scene_runtime_pose_scene_unavailable",
                  current_pose.error().message.empty()
                      ? "logical-scene object pose is unavailable"
                      : current_pose.error().message,
                  {{"scene_error_code", current_pose.error().code}}));
      }
      if (current_pose.value() == decoded.pose) {
        if (prior_application.has_value() &&
            prior_application->status ==
                SceneRuntimePoseApplicationStatus::kFreshnessConfirmed) {
          // This exact observation was already confirmed; a replay reports
          // the outcome again without refreshing freshness age.
          auto result = SceneRuntimePoseApplicationResult{
              SceneRuntimePoseApplicationStatus::kFreshnessConfirmed,
              request_id, projection, std::nullopt};
          return core::Result<SceneRuntimePoseApplicationResult>::Ok(
              std::move(result));
        }
        core::Result<core::ProjectionRevisionDTO> confirmed;
        try {
          confirmed = scene_authority_.ConfirmProviderFreshness(
              core::ProviderFreshnessConfirmationDTO{
                  projection,
                  core::SceneResourceId(observation.key.object_id),
                  decoded.pose,
                  evidence,
              });
        } catch (const std::exception &error) {
          return core::Result<SceneRuntimePoseApplicationResult>::Err(
              Error("scene_runtime_pose_scene_unavailable", error.what()));
        }
        if (confirmed.ok()) {
          if (confirmed.value() != projection) {
            return core::Result<SceneRuntimePoseApplicationResult>::Err(Error(
                "scene_runtime_pose_freshness_confirmation_invalid",
                "logical-scene owner changed the projection revision of a "
                "freshness-only confirmation"));
          }
          auto result = SceneRuntimePoseApplicationResult{
              SceneRuntimePoseApplicationStatus::kFreshnessConfirmed,
              request_id, projection, std::nullopt};
          return core::Result<SceneRuntimePoseApplicationResult>::Ok(
              std::move(result));
        }
        if (confirmed.error().code != kProviderFreshnessUnsupported) {
          // No Commit fallback: surface the owner's refusal with its complete
          // details (including failed, applied and uncertain projections of a
          // partial propagation) instead of a revision-advancing write.
          auto details = confirmed.error().details;
          details["scene_error_code"] = confirmed.error().code;
          return core::Result<SceneRuntimePoseApplicationResult>::Err(
              Error("scene_runtime_pose_freshness_confirmation_failed",
                    confirmed.error().message.empty()
                        ? "logical-scene owner rejected the freshness "
                          "confirmation"
                        : confirmed.error().message,
                    std::move(details)));
        }
        // The authority has no freshness-only path: keep today's commit.
      }
    }
  }

  core::SceneCommitResultDTO committed{
      request_id,
      core::RequestReplayStatusDTO::kAcceptedNew,
      false,
      Error("scene_runtime_pose_scene_unavailable",
            "logical-scene owner was not called"),
      std::nullopt,
      std::nullopt,
      core::SceneProjectionCommitStatusDTO::kNotAttempted,
      std::nullopt,
  };
  try {
    const core::SceneTransactionRequestDTO request{
        core::PublicRequestContextDTO{
            request_id,
            scene_authority_.CurrentRequestOwnerEpoch(),
            "scene_runtime_pose_source",
            request_id.value(),
        },
        {},
        projection,
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId(observation.key.object_id),
            decoded.pose,
            core::CellSceneFrameRefDTO{cell_id_},
            evidence,
            false,
        },
        std::nullopt,
    };
    committed = scene_authority_.Commit(request);
  } catch (const std::exception &error) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_scene_unavailable", error.what()));
  }
  if (!committed.presence_valid()) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_commit_invalid",
              "logical-scene owner returned an invalid pose-update result"));
  }
  if (!committed.success) {
    std::map<std::string, std::string> details{
        {"projection_status",
         std::string(core::to_string(committed.projection_status))},
        {"replay_status",
         std::string(core::to_string(committed.replay_status))},
    };
    if (committed.error.has_value()) {
      details.emplace("scene_error_code", committed.error->code);
    }
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_commit_failed",
              committed.error.has_value()
                  ? committed.error->message
                  : "logical-scene owner rejected the pose update",
              std::move(details)));
  }
  auto result = SceneRuntimePoseApplicationResult{
      SceneRuntimePoseApplicationStatus::kCommitted, request_id,
      *committed.committed_projection_revision, committed.update_status};
  if (!result.valid()) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(Error(
        "scene_runtime_pose_commit_invalid",
        "logical-scene owner returned an incomplete pose-update success"));
  }
  return core::Result<SceneRuntimePoseApplicationResult>::Ok(std::move(result));
}

} // namespace manyforge::headless
