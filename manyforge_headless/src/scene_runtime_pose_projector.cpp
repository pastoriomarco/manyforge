#include "manyforge_headless/scene_runtime_pose_projector.hpp"

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/geometry/pose_math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using content_identity::CanonicalValue;

constexpr std::string_view kPoseObservationPayloadApiVersion =
    "manyforge.ros_pose_observation.v2";
// A simulation entity pose that also carries the object pose relative to a
// gripper reference entity read in the same source snapshot.
constexpr std::string_view kReferencedSimulationPoseKind =
    "simulation_entity_pose_with_reference.v1";
constexpr std::string_view kRelativeToAttachedPolicy =
    "continuous_relative_to_attached";
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

struct DecodedReference final {
  std::string link;
  bool coherent{};
  std::optional<core::Pose3DTO> pose_in_reference;
};

struct DecodedPose final {
  core::Pose3DTO pose;
  std::optional<std::int64_t> source_timestamp_ns;
  std::optional<DecodedReference> reference;
};

core::Pose3DTO DecodeRigidPose(const CanonicalValue &pose,
                               const std::string &label) {
  const auto &position =
      Array(Member(pose, "position_m"), 3U, label + " position_m");
  const auto &orientation =
      Array(Member(pose, "orientation_xyzw"), 4U, label + " orientation_xyzw");
  core::Pose3DTO result{
      {Number(position[0], label + " position x"),
       Number(position[1], label + " position y"),
       Number(position[2], label + " position z")},
      {Number(orientation[0], label + " orientation x"),
       Number(orientation[1], label + " orientation y"),
       Number(orientation[2], label + " orientation z"),
       Number(orientation[3], label + " orientation w")},
  };
  const auto norm_squared = result.orientation.x * result.orientation.x +
                            result.orientation.y * result.orientation.y +
                            result.orientation.z * result.orientation.z +
                            result.orientation.w * result.orientation.w;
  if (!std::isfinite(norm_squared) || norm_squared <= 0.0 ||
      std::abs(norm_squared - 1.0) > 1.0e-6) {
    throw std::invalid_argument(label +
                                " orientation is not one unit quaternion");
  }
  const auto inverse_norm = 1.0 / std::sqrt(norm_squared);
  result.orientation.x *= inverse_norm;
  result.orientation.y *= inverse_norm;
  result.orientation.z *= inverse_norm;
  result.orientation.w *= inverse_norm;
  return result;
}

DecodedReference DecodeReference(const CanonicalValue &reference) {
  ExactObject(reference, 4U, "pose reference");
  static_cast<void>(Text(Member(reference, "entity"), "reference entity"));
  const auto *coherent =
      std::get_if<bool>(&Member(reference, "coherent").storage());
  if (coherent == nullptr) {
    throw std::invalid_argument("reference coherent must be a boolean");
  }
  DecodedReference result{Text(Member(reference, "link"), "reference link"),
                          *coherent, std::nullopt};
  const auto &pose = Member(reference, "pose_in_reference");
  if (!std::holds_alternative<std::nullptr_t>(pose.storage())) {
    ExactObject(pose, 2U, "reference pose");
    result.pose_in_reference = DecodeRigidPose(pose, "reference pose");
  }
  return result;
}

DecodedPose DecodePose(const CanonicalValue &payload) {
  const auto *kind_value = payload.find_object_member("kind");
  const bool referenced =
      kind_value != nullptr && kind_value->is_string() &&
      kind_value->as_string() == kReferencedSimulationPoseKind;
  ExactObject(payload, referenced ? 5U : 4U, "pose observation payload");
  if (Text(Member(payload, "api_version"), "pose payload api_version") !=
      kPoseObservationPayloadApiVersion) {
    throw std::invalid_argument("pose payload api_version is unsupported");
  }
  const auto &kind = Text(Member(payload, "kind"), "pose payload kind");
  ValidateSource(referenced ? std::string_view("simulation_entity_pose")
                            : std::string_view(kind),
                 Member(payload, "source"));
  const auto &pose = Member(payload, "pose");
  ExactObject(pose, 5U, "pose payload pose");
  if (Text(Member(pose, "frame_kind"), "pose frame_kind") != "cell_frame") {
    throw std::invalid_argument(
        "pose payload is not in the semantic cell frame");
  }
  static_cast<void>(Text(Member(pose, "frame_id"), "pose frame_id"));
  return {DecodeRigidPose(pose, "pose"),
          SourceTimestampNs(Member(pose, "source_stamp_ns")),
          referenced ? std::optional(DecodeReference(Member(payload, "reference")))
                     : std::nullopt};
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

// URDF origin: translation, then fixed-axis roll, pitch and yaw.
core::Pose3DTO JointOrigin(const deployment::UrdfOriginIr &origin) {
  const auto half = [&](const std::size_t axis) {
    return 0.5 * origin.rpy_rad[axis];
  };
  const double cr = std::cos(half(0)), sr = std::sin(half(0));
  const double cp = std::cos(half(1)), sp = std::sin(half(1));
  const double cy = std::cos(half(2)), sy = std::sin(half(2));
  return {{origin.xyz_m[0], origin.xyz_m[1], origin.xyz_m[2]},
          {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
           cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy}};
}

// The observed reference-relative object pose composed onto the attachment's
// carrier frame. The relative pose must come from one source instant.
core::Result<std::pair<core::Pose3DTO, core::ComponentSceneFrameRefDTO>>
RelativeCarrierPose(const core::ISceneAuthority &scene_authority,
                    const SceneRuntimeReferenceLinkResolver &resolver,
                    const core::ProjectionRevisionDTO &projection,
                    const core::SceneObjectStateDTO &object_state,
                    const DecodedPose &decoded) {
  using Reply =
      core::Result<std::pair<core::Pose3DTO, core::ComponentSceneFrameRefDTO>>;
  if (!decoded.reference.has_value()) {
    return Reply::Err(Error(
        "scene_runtime_pose_reference_missing",
        "relative attached pose update requires an observation relative to "
        "a gripper reference",
        {{"scene_resource_id", object_state.scene_resource_id.value()}}));
  }
  const auto &reference = *decoded.reference;
  if (!reference.pose_in_reference.has_value()) {
    return Reply::Err(Error("scene_runtime_pose_reference_unavailable",
                            "source has no reference-relative object pose",
                            {{"reference_link", reference.link}}));
  }
  if (!reference.coherent) {
    return Reply::Err(Error(
        "scene_runtime_pose_reference_not_coherent",
        "source did not read object and reference at one instant",
        {{"reference_link", reference.link}}));
  }
  const auto snapshot = scene_authority.LogicalSnapshot();
  if (snapshot == nullptr ||
      snapshot->revision != projection.scene_revision) {
    return Reply::Err(
        Error("scene_runtime_pose_projection_changed",
              "logical-scene snapshot differs from the admitted projection"));
  }
  const auto attachment = std::find_if(
      snapshot->carried_object_attachments.begin(),
      snapshot->carried_object_attachments.end(), [&](const auto &entry) {
        return object_state.carried_object_attachment_id ==
                   entry.carried_object_attachment_id &&
               entry.scene_resource_id == object_state.scene_resource_id;
      });
  if (attachment == snapshot->carried_object_attachments.end()) {
    return Reply::Err(Error(
        "scene_runtime_pose_scene_unavailable",
        "attached pose-source object has no exact carried attachment",
        {{"scene_resource_id", object_state.scene_resource_id.value()}}));
  }
  const auto *carrier =
      std::get_if<core::ComponentSceneFrameRefDTO>(&attachment->carrier_frame_ref);
  if (carrier == nullptr || !resolver) {
    return Reply::Err(Error(
        "scene_runtime_pose_reference_unresolved",
        "attachment carrier has no retained-model reference resolution",
        {{"reference_link", reference.link}}));
  }
  auto carrier_in_reference = resolver(projection.retained_bundle_id, *carrier,
                                       reference.link);
  if (!carrier_in_reference.ok()) {
    return Reply::Err(carrier_in_reference.error());
  }
  return Reply::Ok(std::make_pair(
      core::multiply_poses(core::inverse_pose(carrier_in_reference.value()),
                           *reference.pose_in_reference),
      *carrier));
}

} // namespace

core::Result<core::Pose3DTO>
SceneRuntimeFixedLinkTransform(const deployment::UrdfDocumentIr &urdf,
                               const std::string_view reference_link,
                               const std::string_view target_link) {
  using Reply = core::Result<core::Pose3DTO>;
  const auto known = [&](const std::string_view link) {
    return std::any_of(urdf.links.begin(), urdf.links.end(),
                       [&](const auto &entry) { return entry.name == link; });
  };
  if (!known(reference_link) || !known(target_link)) {
    return Reply::Err(Error("scene_runtime_pose_reference_link_unknown",
                            "reference or carrier link is not in the model",
                            {{"reference_link", std::string(reference_link)},
                             {"carrier_link", std::string(target_link)}}));
  }
  std::map<std::string_view, const deployment::UrdfJointIr *> parent_joints;
  for (const auto &joint : urdf.joints) {
    parent_joints.emplace(joint.child_link_name, &joint);
  }
  struct Step final {
    std::string_view link;
    core::Pose3DTO start_in_link;
    bool fixed;
  };
  // Each start link's chain to the model root: its pose in every ancestor and
  // whether only fixed joints lie between them.
  const auto chain = [&](const std::string_view start) {
    std::vector<Step> steps{{start, core::Pose3DTO{}, true}};
    for (auto found = parent_joints.find(start);
         found != parent_joints.end() && steps.size() <= urdf.joints.size();
         found = parent_joints.find(steps.back().link)) {
      const auto &joint = *found->second;
      steps.push_back(
          {joint.parent_link_name,
           core::multiply_poses(JointOrigin(joint.origin),
                                steps.back().start_in_link),
           steps.back().fixed &&
               joint.type == deployment::UrdfJointType::kFixed});
    }
    return steps;
  };
  const auto from_reference = chain(reference_link);
  for (const auto &target : chain(target_link)) {
    const auto common = std::find_if(
        from_reference.begin(), from_reference.end(),
        [&](const Step &step) { return step.link == target.link; });
    if (common == from_reference.end()) {
      continue;
    }
    if (!common->fixed || !target.fixed) {
      break;
    }
    return Reply::Ok(core::multiply_poses(
        core::inverse_pose(common->start_in_link), target.start_in_link));
  }
  return Reply::Err(Error("scene_runtime_pose_reference_not_fixed",
                          "no fixed model path joins the reference link and "
                          "the carrier frame",
                          {{"reference_link", std::string(reference_link)},
                           {"carrier_link", std::string(target_link)}}));
}

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
  case SceneRuntimePoseApplicationStatus::kDeferredBetweenNodes:
    return "deferred_between_nodes";
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
  case SceneRuntimePoseApplicationStatus::kDeferredBetweenNodes:
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
  case SceneRuntimePoseApplicationStatus::kDeferredBetweenNodes:
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
    core::ISceneAuthority &scene_authority, core::CellId cell_id,
    SceneRuntimeReferenceLinkResolver reference_resolver)
    : scene_authority_(scene_authority), cell_id_(std::move(cell_id)),
      reference_resolver_(std::move(reference_resolver)) {
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
    const std::optional<SceneRuntimePoseApplicationResult> &prior_application,
    const std::string_view apply_policy) {
  const auto policy = apply_policy.empty()
                          ? std::string_view(observation.key.policy_id)
                          : apply_policy;
  if (!observation.valid() || observation.source_epoch_order == 0U ||
      observation.source_sequence == 0U ||
      observation.payload_quality != PoseSourceQuality::kGood ||
      !observation.eligible ||
      (policy != "continuous_unattached" && policy != "continuous" &&
       policy != kRelativeToAttachedPolicy && policy != "before_planning" &&
       policy != "between_nodes")) {
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
  if (policy == "before_planning" || policy == "between_nodes") {
    // A safe-point capture is a planning snapshot, not a continuous feed.
    evidence.source_timestamp_ns.reset();
    evidence.source_freshness_budget_ns.reset();
  }
  if (!evidence.valid()) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_evidence_invalid",
              "derived pose-provider evidence is invalid"));
  }

  auto request_id =
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
  if (object_state.attached &&
      (policy == "before_planning" || policy == "continuous_unattached")) {
    auto result = SceneRuntimePoseApplicationResult{
        SceneRuntimePoseApplicationStatus::kSkippedAttached, request_id,
        projection, std::nullopt};
    return core::Result<SceneRuntimePoseApplicationResult>::Ok(
        std::move(result));
  }

  if (object_state.attached &&
      object_state.provider_evidence == std::optional(evidence) &&
      prior_application && prior_application->valid() &&
      (prior_application->status ==
           SceneRuntimePoseApplicationStatus::kCommitted ||
       prior_application->status ==
           SceneRuntimePoseApplicationStatus::kAlreadyApplied)) {
    // The Manager supplied this exact observation's retained success. A carrier
    // may have moved since; never reapply the same old world observation to it.
    return core::Result<SceneRuntimePoseApplicationResult>::Ok(
        {SceneRuntimePoseApplicationStatus::kAlreadyApplied, request_id,
         projection, std::nullopt});
  }
  if (!object_state.attached && object_state.provider_evidence.has_value() &&
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
    const bool safe_point_policy =
        policy == "before_planning" || policy == "between_nodes";
    if (evidence.provider_source_sequence >
            previous.provider_source_sequence &&
        safe_point_policy && !evidence.source_timestamp_ns.has_value() &&
        !evidence.source_freshness_budget_ns.has_value() &&
        !previous.source_timestamp_ns.has_value() &&
        !previous.source_freshness_budget_ns.has_value() &&
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
        auto result = SceneRuntimePoseApplicationResult{
            SceneRuntimePoseApplicationStatus::kAlreadyApplied, request_id,
            projection, std::nullopt};
        return core::Result<SceneRuntimePoseApplicationResult>::Ok(
            std::move(result));
      }
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
    if (evidence.source_freshness_budget_ns.has_value() &&
        previous.source_freshness_budget_ns ==
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

  core::Pose3DTO update_pose = decoded.pose;
  core::SceneTransactionFrameRefDTO update_frame =
      core::CellSceneFrameRefDTO{cell_id_};
  std::optional<bool> expected_attached =
      (policy == "continuous" || policy == "between_nodes")
          ? std::nullopt
          : std::optional(false);
  if (object_state.attached && policy == kRelativeToAttachedPolicy) {
    core::Result<std::pair<core::Pose3DTO, core::ComponentSceneFrameRefDTO>>
        carried;
    try {
      carried = RelativeCarrierPose(scene_authority_, reference_resolver_,
                                    projection, object_state, decoded);
    } catch (const std::exception &error) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          Error("scene_runtime_pose_scene_unavailable", error.what()));
    }
    if (!carried.ok()) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          carried.error());
    }
    update_pose = carried.value().first;
    update_frame = carried.value().second;
    expected_attached = true;
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
    core::SceneTransactionRequestDTO request{
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
            update_pose,
            update_frame,
            evidence,
            expected_attached,
        },
        std::nullopt,
    };
    committed = scene_authority_.Commit(request);
    // Only this exact, pre-acceptance housekeeping refusal permits one new
    // request. An old-epoch rejection without this proof is not replayable.
    if (committed.presence_valid() && !committed.success && committed.error &&
        committed.replay_status == core::RequestReplayStatusDTO::kStaleRequestOwnerEpoch &&
        committed.request_id == request.request_context.request_id &&
        committed.projection_status == core::SceneProjectionCommitStatusDTO::kNotAttempted &&
        !committed.transaction_sequence &&
        committed.error->code == "stale_request_owner_epoch") {
      const auto &details = committed.error->details;
      const auto reason = details.find("reason");
      const auto previous = details.find("previous_request_owner_epoch");
      const auto next = details.find("expected_request_owner_epoch");
      if (reason != details.end() && reason->second == "scene_history_compaction_no_effect" &&
          previous != details.end() && next != details.end() &&
          previous->second == request.request_context.request_owner_epoch.value() &&
          next->second != previous->second && !next->second.empty() &&
          scene_authority_.CurrentProjectionRevision() == projection &&
          scene_authority_.CurrentRequestOwnerEpoch().value() == next->second) {
        request.request_context.request_owner_epoch = core::RequestOwnerEpoch(next->second);
        request_id = core::RequestId(request_id.value() + ".after-compaction");
        request.request_context.request_id = request_id;
        committed = scene_authority_.Commit(request);
      }
    }
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
