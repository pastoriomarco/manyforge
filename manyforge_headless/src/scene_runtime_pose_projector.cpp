#include "manyforge_headless/scene_runtime_pose_projector.hpp"

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/types/carried_object_lost.hpp>

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
  static_cast<void>(SourceTimestampNs(Member(pose, "source_stamp_ns")));
  return {DecodeRigidPose(pose, "pose"),
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
                 {"observed_at_ns",
                  Integer(observation.observed_at_ns, "observed_at_ns")},
                 {"provider_source_epoch",
                  CanonicalValue(evidence.provider_source_epoch)},
                 {"source_epoch", CanonicalValue(observation.source_epoch)},
                 {"source_epoch_order", Unsigned(observation.source_epoch_order,
                                                 "source_epoch_order")},
                 {"source_id", CanonicalValue(observation.key.source_id)},
                 {"source_sequence",
                  Unsigned(observation.source_sequence, "source_sequence")},
             })));
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

// The attachment row of an attached object in the admitted projection.
core::Result<core::CarriedObjectAttachmentDTO>
CurrentAttachment(const core::ISceneAuthority &scene_authority,
                  const core::ProjectionRevisionDTO &projection,
                  const core::SceneObjectStateDTO &object_state) {
  using Reply = core::Result<core::CarriedObjectAttachmentDTO>;
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
  return Reply::Ok(*attachment);
}

// Unchanged within the deadband; a zero bound compares that axis exactly.
bool WithinDeadband(const core::Pose3DTO &current,
                    const core::Pose3DTO &observed,
                    const SceneRuntimePoseDeadband &deadband) {
  if (current == observed) {
    return true;
  }
  const auto distance = SceneRuntimePoseDistanceBetween(current, observed);
  return (deadband.translation_m > 0.0
              ? distance.translation_m <= deadband.translation_m
              : current.position_m == observed.position_m) &&
         (deadband.rotation_rad > 0.0
              ? distance.rotation_rad <= deadband.rotation_rad
              : current.orientation == observed.orientation);
}

struct CarrierPose final {
  core::Pose3DTO observed_in_carrier;
  core::CarriedObjectAttachmentDTO attachment;
  core::ComponentSceneFrameRefDTO carrier;
};

// The observed reference-relative object pose composed onto the attachment's
// carrier frame, with the attachment row. The relative pose must come from
// one source instant.
core::Result<CarrierPose>
RelativeCarrierPose(const core::ISceneAuthority &scene_authority,
                    const SceneRuntimeReferenceLinkResolver &resolver,
                    const core::ProjectionRevisionDTO &projection,
                    const core::SceneObjectStateDTO &object_state,
                    const DecodedPose &decoded) {
  using Reply = core::Result<CarrierPose>;
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
  auto attachment = CurrentAttachment(scene_authority, projection, object_state);
  if (!attachment.ok()) {
    return Reply::Err(attachment.error());
  }
  const auto *carrier = std::get_if<core::ComponentSceneFrameRefDTO>(
      &attachment.value().carrier_frame_ref);
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
  return Reply::Ok(CarrierPose{
      core::multiply_poses(core::inverse_pose(carrier_in_reference.value()),
                           *reference.pose_in_reference),
      attachment.value(), *carrier});
}

} // namespace

SceneRuntimePoseDistance
SceneRuntimePoseDistanceBetween(const core::Pose3DTO &from,
                                const core::Pose3DTO &to) noexcept {
  const auto dx = to.position_m.x_m - from.position_m.x_m;
  const auto dy = to.position_m.y_m - from.position_m.y_m;
  const auto dz = to.position_m.z_m - from.position_m.z_m;
  const auto &a = from.orientation;
  const auto &b = to.orientation;
  const auto dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) /
                   std::sqrt((a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w) *
                             (b.x * b.x + b.y * b.y + b.z * b.z + b.w * b.w));
  return {std::sqrt(dx * dx + dy * dy + dz * dz),
          2.0 * std::acos(std::min(1.0, dot))};
}

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
  case SceneRuntimePoseApplicationStatus::kUnchanged:
    return "unchanged";
  }
  return {};
}

bool SceneRuntimePoseApplicationRetainsPlanningEvidence(
    const SceneRuntimePoseApplicationStatus status) noexcept {
  switch (status) {
  case SceneRuntimePoseApplicationStatus::kCommitted:
  case SceneRuntimePoseApplicationStatus::kAlreadyApplied:
  case SceneRuntimePoseApplicationStatus::kUnchanged:
    return true;
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
  case SceneRuntimePoseApplicationStatus::kUnchanged:
    return scene_request_id.has_value() && !scene_request_id->empty() &&
           projection_revision.has_value() && projection_revision->valid() &&
           !update_status.has_value();
  }
  return false;
}

SceneRuntimePoseProjector::SceneRuntimePoseProjector(
    core::ISceneAuthority &scene_authority, core::CellId cell_id,
    SceneRuntimeReferenceLinkResolver reference_resolver,
    SceneRuntimeCarrierStopRequester carrier_stop_requester)
    : scene_authority_(scene_authority), cell_id_(std::move(cell_id)),
      reference_resolver_(std::move(reference_resolver)),
      carrier_stop_requester_(std::move(carrier_stop_requester)) {
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
    const std::string_view apply_policy,
    const SceneRuntimePoseDeadband deadband) {
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

  // The logical scene carries the provider identity only; the observation's
  // sequence and times stay in the scene runtime.
  core::SceneObjectProviderEvidenceDTO evidence{
      provider->provider_instance_id,
      provider->provider_manifest_id,
      provider->provider_manifest_revision,
      SceneRuntimePoseProviderSourceEpoch(observation),
  };
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
  if (!object_state.attached) {
    ForgetGraspBaseline(observation.key.object_id);
  }
  core::Pose3DTO update_pose = decoded.pose;
  core::SceneTransactionFrameRefDTO update_frame =
      core::CellSceneFrameRefDTO{cell_id_};
  std::optional<bool> expected_attached =
      (policy == "continuous" || policy == "between_nodes")
          ? std::nullopt
          : std::optional(false);
  // Observation order (stale or repeated samples) is enforced per source by
  // the scene runtime before an observation reaches this projector.
  const bool same_origin = object_state.provider_evidence == evidence;
  // The object's pose already in the scene in the update frame, when the
  // update frame is the cell or the attachment's carrier.
  std::optional<core::Pose3DTO> current_pose;
  if (object_state.attached && policy == kRelativeToAttachedPolicy) {
    core::Result<CarrierPose> carried;
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
    if (const auto lost = GraspDrift(carried.value().attachment,
                                     carried.value().observed_in_carrier)) {
      return CarriedObjectLost(carried.value().attachment, *lost, projection,
                               request_id, decoded.pose);
    }
    update_pose = carried.value().observed_in_carrier;
    update_frame = carried.value().carrier;
    current_pose = carried.value().attachment.pose_in_carrier_frame;
    expected_attached = true;
  } else if (object_state.attached && policy == "continuous") {
    // The cell-frame observation expressed on the carrier through the
    // object's current resolved pose: the measured carried offset. Without
    // an exact current attachment row this sample is applied unchecked, as
    // before; the scene owner resolves the carrier itself.
    try {
      const auto attachment =
          CurrentAttachment(scene_authority_, projection, object_state);
      if (attachment.ok() && attachment.value().grasp_lost_check) {
        const auto object_pose = scene_authority_.GetObjectPose(
            core::SceneResourceId(observation.key.object_id));
        if (!object_pose.ok()) {
          return core::Result<SceneRuntimePoseApplicationResult>::Err(Error(
              "scene_runtime_pose_scene_unavailable",
              object_pose.error().message,
              {{"scene_error_code", object_pose.error().code}}));
        }
        const auto measured = core::multiply_poses(
            core::multiply_poses(attachment.value().pose_in_carrier_frame,
                                 core::inverse_pose(object_pose.value())),
            decoded.pose);
        if (const auto lost = GraspDrift(attachment.value(), measured)) {
          return CarriedObjectLost(attachment.value(), *lost, projection,
                                   request_id, decoded.pose);
        }
      }
    } catch (const std::exception &error) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          Error("scene_runtime_pose_scene_unavailable", error.what()));
    }
  } else if (!object_state.attached && same_origin) {
    core::Result<core::Pose3DTO> cell_pose;
    try {
      cell_pose = scene_authority_.GetObjectPose(
          core::SceneResourceId(observation.key.object_id));
    } catch (const std::exception &error) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          Error("scene_runtime_pose_scene_unavailable", error.what()));
    }
    if (!cell_pose.ok()) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          Error("scene_runtime_pose_scene_unavailable",
                cell_pose.error().message.empty()
                    ? "logical-scene object pose is unavailable"
                    : cell_pose.error().message,
                {{"scene_error_code", cell_pose.error().code}}));
    }
    current_pose = cell_pose.value();
  }
  // A same-origin sample of the pose already in the scene, within the
  // binding's deadband (exact by default), changes nothing: no transaction,
  // no new revision.
  if (same_origin && current_pose &&
      WithinDeadband(*current_pose, update_pose, deadband)) {
    return core::Result<SceneRuntimePoseApplicationResult>::Ok(
        {SceneRuntimePoseApplicationStatus::kUnchanged, request_id, projection,
         std::nullopt});
  }

  std::optional<core::SceneCommitResultDTO> submitted;
  try {
    submitted = Commit(request_id, projection,
                       core::UpdateObjectPoseRequestDTO{
                           core::SceneResourceId(observation.key.object_id),
                           update_pose,
                           update_frame,
                           evidence,
                           expected_attached,
                       });
  } catch (const std::exception &error) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(
        Error("scene_runtime_pose_scene_unavailable", error.what()));
  }
  const auto &committed = *submitted;
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
      SceneRuntimePoseApplicationStatus::kCommitted, committed.request_id,
      *committed.committed_projection_revision, committed.update_status};
  if (!result.valid()) {
    return core::Result<SceneRuntimePoseApplicationResult>::Err(Error(
        "scene_runtime_pose_commit_invalid",
        "logical-scene owner returned an incomplete pose-update success"));
  }
  return core::Result<SceneRuntimePoseApplicationResult>::Ok(std::move(result));
}

core::SceneCommitResultDTO
SceneRuntimePoseProjector::Commit(core::RequestId request_id,
                                  const core::ProjectionRevisionDTO &projection,
                                  core::SceneOperationRequestDTO operation) {
  core::SceneTransactionRequestDTO request{
      core::PublicRequestContextDTO{
          request_id,
          scene_authority_.CurrentRequestOwnerEpoch(),
          "scene_runtime_pose_source",
          request_id.value(),
      },
      {},
      projection,
      std::move(operation),
      std::nullopt,
  };
  auto committed = scene_authority_.Commit(request);
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
  return committed;
}

std::optional<SceneRuntimePoseDistance> SceneRuntimePoseProjector::GraspDrift(
    const core::CarriedObjectAttachmentDTO &attachment,
    const core::Pose3DTO &measured_in_carrier) {
  if (!attachment.grasp_lost_check) {
    return std::nullopt;
  }
  // The baseline is the first measured carried offset of this attachment, so
  // the comparison never mixes the stream's frames with the scene's own
  // attach-time offset (they can differ by a fixed frame or symmetry offset
  // plus the real grasp error).
  std::lock_guard lock(grasp_baselines_mutex_);
  const auto found = grasp_baselines_.find(attachment.scene_resource_id.value());
  if (found == grasp_baselines_.end() ||
      found->second.carried_object_attachment_id !=
          attachment.carried_object_attachment_id) {
    grasp_baselines_.insert_or_assign(
        attachment.scene_resource_id.value(),
        GraspBaseline{attachment.carried_object_attachment_id,
                      measured_in_carrier});
    return std::nullopt;
  }
  const auto &bound = attachment.grasp_lost_check->bound;
  const auto distance = SceneRuntimePoseDistanceBetween(
      found->second.measured_in_carrier, measured_in_carrier);
  if (distance.translation_m <= bound.translation_m &&
      distance.rotation_rad <= bound.rotation_rad) {
    return std::nullopt;
  }
  return distance;
}

void SceneRuntimePoseProjector::ForgetGraspBaseline(
    const std::string &scene_resource_id) {
  std::lock_guard lock(grasp_baselines_mutex_);
  grasp_baselines_.erase(scene_resource_id);
}

core::Result<SceneRuntimePoseApplicationResult>
SceneRuntimePoseProjector::CarriedObjectLost(
    const core::CarriedObjectAttachmentDTO &attachment,
    const SceneRuntimePoseDistance &distance,
    const core::ProjectionRevisionDTO &projection,
    const core::RequestId &request_id,
    const core::Pose3DTO &observed_in_cell) {
  ForgetGraspBaseline(attachment.scene_resource_id.value());
  const auto &bound = attachment.grasp_lost_check->bound;
  std::map<std::string, std::string> details{
      {"scene_resource_id", attachment.scene_resource_id.value()},
      {"carried_object_attachment_id",
       attachment.carried_object_attachment_id.value()},
      {"controllable_component_id",
       attachment.controllable_component_id.value()},
      {"translation_m", std::to_string(distance.translation_m)},
      {"rotation_rad", std::to_string(distance.rotation_rad)},
      {"bound_translation_m", std::to_string(bound.translation_m)},
      {"bound_rotation_rad", std::to_string(bound.rotation_rad)},
      {"event_id", request_id.value()},
  };
  // 1. Stop the carrier's motion first, so the next motion leaves the
  //    contact through the start-contact allowance instead of a misleading
  //    runtime-collision Stop.
  if (carrier_stop_requester_) {
    const auto refused = carrier_stop_requester_(SceneRuntimeCarriedObjectLoss{
        attachment.scene_resource_id, attachment.carried_object_attachment_id,
        attachment.controllable_component_id, projection.graph_revision,
        request_id.value() + ".stop"});
    details.emplace("stop", refused ? refused->code : "requested");
  }
  // 2. Detach it as an ordinary world object at the observed pose.
  try {
    const auto detached = Commit(
        core::RequestId(request_id.value() + ".detach"), projection,
        core::DetachCarriedObjectRequestDTO{
            attachment.carried_object_attachment_id,
            attachment.carried_object_attachment_revision, observed_in_cell,
            core::CellSceneFrameRefDTO{cell_id_}});
    details.emplace("detach", detached.success
                                  ? "committed"
                                  : (detached.error ? detached.error->code
                                                    : "rejected"));
  } catch (const std::exception &) {
    details.emplace("detach", "scene_runtime_pose_scene_unavailable");
  }
  return core::Result<SceneRuntimePoseApplicationResult>::Err(Error(
      core::kCarriedObjectLostReason,
      core::CarriedObjectLostFullText(
          attachment.scene_resource_id.value(), distance.translation_m,
          distance.rotation_rad, bound.translation_m, bound.rotation_rad),
      std::move(details)));
}

} // namespace manyforge::headless
