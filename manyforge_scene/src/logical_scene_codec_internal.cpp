#include "logical_scene_codec_internal.hpp"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace manyforge::scene::logical_scene_codec_internal {
namespace {

namespace ci = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using ci::CanonicalValue;

constexpr std::uint64_t kMaximumSafeIntegral = 9007199254740991ULL;

[[noreturn]] void Fail(const std::string &detail) {
  throw std::invalid_argument("logical-scene codec: " + detail);
}

CanonicalValue Unsigned(const std::uint64_t value,
                        const std::string_view label) {
  if (value > kMaximumSafeIntegral) {
    Fail(std::string(label) + " exceeds the safe-integral range");
  }
  return CanonicalValue(static_cast<double>(value));
}

const CanonicalValue &Member(const CanonicalValue &object,
                             const std::string_view name) {
  const auto *value = object.find_object_member(name);
  if (value == nullptr) {
    Fail("missing field " + std::string(name));
  }
  return *value;
}

void ExactObject(const CanonicalValue &value, const std::size_t count,
                 const std::string_view label) {
  if (!value.is_object() || value.as_object().size() != count) {
    Fail(std::string(label) + " has an unknown or missing field");
  }
}

const std::string &String(const CanonicalValue &value,
                          const std::string_view label) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    Fail(std::string(label) + " must be a string");
  }
  return *text;
}

bool Boolean(const CanonicalValue &value, const std::string_view label) {
  const auto *flag = std::get_if<bool>(&value.storage());
  if (flag == nullptr) {
    Fail(std::string(label) + " must be boolean");
  }
  return *flag;
}

double Number(const CanonicalValue &value, const std::string_view label) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    Fail(std::string(label) + " must be finite numeric data");
  }
  return *number;
}

std::uint64_t DecodeUnsigned(const CanonicalValue &value,
                             const std::string_view label) {
  const auto number = Number(value, label);
  if (number < 0.0 || number > static_cast<double>(kMaximumSafeIntegral) ||
      std::floor(number) != number) {
    Fail(std::string(label) + " must be an unsigned safe integer");
  }
  return static_cast<std::uint64_t>(number);
}

const CanonicalValue::Array &Array(const CanonicalValue &value,
                                   const std::string_view label) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    Fail(std::string(label) + " must be an array");
  }
  return *array;
}

CanonicalValue StringArray(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

template <typename Strong>
CanonicalValue StrongArray(const std::vector<Strong> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(value.value());
  }
  return CanonicalValue(std::move(result));
}

template <typename Strong>
std::vector<Strong> DecodeStrongArray(const CanonicalValue &value,
                                      const std::string_view label) {
  const auto &values = Array(value, label);
  std::vector<Strong> result;
  result.reserve(values.size());
  for (const auto &entry : values) {
    result.emplace_back(String(entry, label));
  }
  return result;
}

CanonicalValue Vector(const core::Vector3DTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"x_m", CanonicalValue(value.x_m)},
      {"y_m", CanonicalValue(value.y_m)},
      {"z_m", CanonicalValue(value.z_m)},
  });
}

core::Vector3DTO DecodeVector(const CanonicalValue &value) {
  ExactObject(value, 3U, "vector");
  return {
      Number(Member(value, "x_m"), "x_m"),
      Number(Member(value, "y_m"), "y_m"),
      Number(Member(value, "z_m"), "z_m"),
  };
}

CanonicalValue Pose(const core::Pose3DTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"orientation", CanonicalValue(CanonicalValue::Object{
                          {"w", CanonicalValue(value.orientation.w)},
                          {"x", CanonicalValue(value.orientation.x)},
                          {"y", CanonicalValue(value.orientation.y)},
                          {"z", CanonicalValue(value.orientation.z)},
                      })},
      {"position_m", Vector(value.position_m)},
  });
}

core::Pose3DTO DecodePose(const CanonicalValue &value) {
  ExactObject(value, 2U, "pose");
  const auto &orientation = Member(value, "orientation");
  ExactObject(orientation, 4U, "orientation");
  core::Pose3DTO result;
  result.position_m = DecodeVector(Member(value, "position_m"));
  result.orientation = {
      Number(Member(orientation, "x"), "orientation.x"),
      Number(Member(orientation, "y"), "orientation.y"),
      Number(Member(orientation, "z"), "orientation.z"),
      Number(Member(orientation, "w"), "orientation.w"),
  };
  return result;
}

CanonicalValue Shape(const core::CollisionShapeDTO &value) {
  return std::visit(
      [](const auto &shape) -> CanonicalValue {
        using T = std::decay_t<decltype(shape)>;
        if constexpr (std::is_same_v<T, core::BoxShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"box_dimensions_m", Vector(shape.dimensions_m)},
              {"type", CanonicalValue("box")},
          });
        } else if constexpr (std::is_same_v<T, core::SphereShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"sphere_radius_m", CanonicalValue(shape.radius_m)},
              {"type", CanonicalValue("sphere")},
          });
        } else if constexpr (std::is_same_v<T, core::CylinderShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"cylinder_height_m", CanonicalValue(shape.height_m)},
              {"cylinder_radius_m", CanonicalValue(shape.radius_m)},
              {"type", CanonicalValue("cylinder")},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"mesh_resource_uri", CanonicalValue(shape.resource_uri)},
              {"mesh_scale", Vector(shape.scale)},
              {"type", CanonicalValue("mesh")},
          });
        }
      },
      value);
}

core::CollisionShapeDTO DecodeShape(const CanonicalValue &value) {
  const auto type = String(Member(value, "type"), "shape.type");
  if (type == "box") {
    ExactObject(value, 2U, "box shape");
    return core::BoxShapeDTO{DecodeVector(Member(value, "box_dimensions_m"))};
  }
  if (type == "sphere") {
    ExactObject(value, 2U, "sphere shape");
    return core::SphereShapeDTO{
        Number(Member(value, "sphere_radius_m"), "sphere_radius_m")};
  }
  if (type == "cylinder") {
    ExactObject(value, 3U, "cylinder shape");
    return core::CylinderShapeDTO{
        Number(Member(value, "cylinder_radius_m"), "cylinder_radius_m"),
        Number(Member(value, "cylinder_height_m"), "cylinder_height_m"),
    };
  }
  if (type == "mesh") {
    ExactObject(value, 3U, "mesh shape");
    return core::MeshShapeDTO{
        String(Member(value, "mesh_resource_uri"), "mesh_resource_uri"),
        DecodeVector(Member(value, "mesh_scale")),
    };
  }
  Fail("shape.type is unsupported");
}

CanonicalValue Pair(const core::CollisionPairId &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"body_a", CanonicalValue(value.body_a)},
      {"body_b", CanonicalValue(value.body_b)},
  });
}

core::CollisionPairId DecodePair(const CanonicalValue &value) {
  ExactObject(value, 2U, "collision pair");
  return {
      String(Member(value, "body_a"), "body_a"),
      String(Member(value, "body_b"), "body_b"),
  };
}

CanonicalValue Pairs(const std::vector<core::CollisionPairId> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(Pair(value));
  }
  return CanonicalValue(std::move(result));
}

std::vector<core::CollisionPairId> DecodePairs(const CanonicalValue &value) {
  std::vector<core::CollisionPairId> result;
  const auto &values = Array(value, "collision pairs");
  result.reserve(values.size());
  for (const auto &entry : values) {
    result.emplace_back(DecodePair(entry));
  }
  return result;
}

std::string_view ContactMode(const core::CollisionContactPolicyMode mode) {
  switch (mode) {
  case core::CollisionContactPolicyMode::kContactRelease:
    return "contact_release";
  case core::CollisionContactPolicyMode::kBoundedPenetration:
    return "bounded_penetration";
  case core::CollisionContactPolicyMode::kContactProcess:
    return "contact_process";
  }
  return {};
}

core::CollisionContactPolicyMode DecodeContactMode(std::string_view mode) {
  if (mode == "contact_release") {
    return core::CollisionContactPolicyMode::kContactRelease;
  }
  if (mode == "bounded_penetration") {
    return core::CollisionContactPolicyMode::kBoundedPenetration;
  }
  if (mode == "contact_process") {
    return core::CollisionContactPolicyMode::kContactProcess;
  }
  Fail("collision contact mode is unsupported");
}

std::string_view ReleaseMode(const core::ContactReleaseEvidenceMode mode) {
  switch (mode) {
  case core::ContactReleaseEvidenceMode::kSignedPenetration:
    return "signed_penetration";
  case core::ContactReleaseEvidenceMode::kBinaryContact:
    return "binary_contact";
  }
  return {};
}

core::ContactReleaseEvidenceMode DecodeReleaseMode(std::string_view mode) {
  if (mode == "signed_penetration") {
    return core::ContactReleaseEvidenceMode::kSignedPenetration;
  }
  if (mode == "binary_contact") {
    return core::ContactReleaseEvidenceMode::kBinaryContact;
  }
  Fail("contact release evidence mode is unsupported");
}

CanonicalValue SupportDeclaration(const core::SupportContactDTO &contact) {
  return CanonicalValue(CanonicalValue::Object{
      {"allow_recontact", CanonicalValue(contact.allow_recontact)},
      {"enforce_max_start_penetration",
       CanonicalValue(contact.enforce_max_start_penetration)},
      {"max_contact_penetration_m",
       CanonicalValue(contact.max_contact_penetration_m)},
      {"max_start_penetration_m",
       CanonicalValue(contact.max_start_penetration_m)},
      {"min_release_progress_m",
       CanonicalValue(contact.min_release_progress_m)},
      {"mode", CanonicalValue(std::string(ContactMode(contact.mode)))},
      {"release_evidence_mode",
       CanonicalValue(std::string(ReleaseMode(contact.release_evidence_mode)))},
      {"release_tolerance_m", CanonicalValue(contact.release_tolerance_m)},
      {"require_separation_by_end",
       CanonicalValue(contact.require_separation_by_end)},
      {"require_strictly_decreasing_penetration",
       CanonicalValue(contact.require_strictly_decreasing_penetration)},
      {"support_object_id", CanonicalValue(contact.support_object_id)},
  });
}

CanonicalValue
SupportDeclarations(const std::vector<core::SupportContactDTO> &contacts) {
  CanonicalValue::Array result;
  result.reserve(contacts.size());
  for (const auto &contact : contacts) {
    result.emplace_back(SupportDeclaration(contact));
  }
  return CanonicalValue(std::move(result));
}

core::SupportContactDTO DecodeSupportDeclaration(const CanonicalValue &value) {
  ExactObject(value, 11U, "support declaration");
  return {
      String(Member(value, "support_object_id"), "support_object_id"),
      DecodeContactMode(String(Member(value, "mode"), "support mode")),
      DecodeReleaseMode(String(Member(value, "release_evidence_mode"),
                               "support release evidence mode")),
      Boolean(Member(value, "enforce_max_start_penetration"),
              "enforce_max_start_penetration"),
      Number(Member(value, "max_start_penetration_m"),
             "max_start_penetration_m"),
      Number(Member(value, "release_tolerance_m"), "release_tolerance_m"),
      Number(Member(value, "min_release_progress_m"), "min_release_progress_m"),
      Number(Member(value, "max_contact_penetration_m"),
             "max_contact_penetration_m"),
      Boolean(Member(value, "require_separation_by_end"),
              "require_separation_by_end"),
      Boolean(Member(value, "require_strictly_decreasing_penetration"),
              "require_strictly_decreasing_penetration"),
      Boolean(Member(value, "allow_recontact"), "allow_recontact"),
  };
}

std::vector<core::SupportContactDTO>
DecodeSupportDeclarations(const CanonicalValue &value) {
  std::vector<core::SupportContactDTO> result;
  const auto &contacts = Array(value, "support_contacts");
  result.reserve(contacts.size());
  for (const auto &contact : contacts) {
    result.emplace_back(DecodeSupportDeclaration(contact));
  }
  return result;
}

CanonicalValue Policy(const core::CollisionPolicySnapshot &policy) {
  CanonicalValue::Array contacts;
  contacts.reserve(policy.support_contacts.size());
  for (const auto &contact : policy.support_contacts) {
    contacts.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"allow_recontact", CanonicalValue(contact.allow_recontact)},
        {"enforce_max_start_penetration",
         CanonicalValue(contact.enforce_max_start_penetration)},
        {"max_contact_penetration_m",
         CanonicalValue(contact.max_contact_penetration_m)},
        {"max_start_penetration_m",
         CanonicalValue(contact.max_start_penetration_m)},
        {"min_release_progress_m",
         CanonicalValue(contact.min_release_progress_m)},
        {"mode", CanonicalValue(std::string(ContactMode(contact.mode)))},
        {"pair", Pair(contact.pair)},
        {"release_evidence_mode", CanonicalValue(std::string(ReleaseMode(
                                      contact.release_evidence_mode)))},
        {"release_tolerance_m", CanonicalValue(contact.release_tolerance_m)},
        {"require_separation_by_end",
         CanonicalValue(contact.require_separation_by_end)},
        {"require_strictly_decreasing_penetration",
         CanonicalValue(contact.require_strictly_decreasing_penetration)},
    }));
  }
  CanonicalValue::Array grasps;
  grasps.reserve(policy.contact_grasps.size());
  for (const auto &grasp : policy.contact_grasps) {
    grasps.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"allowed_robot_links", StringArray(grasp.allowed_robot_links)},
        {"max_penetration_m", CanonicalValue(grasp.max_penetration_m)},
        {"policy_id", CanonicalValue(grasp.policy_id)},
        {"selected_object_id", CanonicalValue(grasp.selected_object_id)},
        {"terminal_window_start_fraction",
         CanonicalValue(grasp.terminal_window_start_fraction)},
    }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"allowed_static_static_pairs",
       Pairs(policy.allowed_static_static_pairs)},
      {"allowed_touch_link_pairs", Pairs(policy.allowed_touch_link_pairs)},
      {"contact_grasps", CanonicalValue(std::move(grasps))},
      {"policy_id", CanonicalValue(policy.policy_id)},
      {"require_full_diagnostics",
       CanonicalValue(policy.require_full_diagnostics)},
      {"revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash", CanonicalValue(policy.revision.content_hash)},
           {"revision_id",
            Unsigned(policy.revision.revision_id, "policy revision")},
       })},
      {"support_contacts", CanonicalValue(std::move(contacts))},
  });
}

core::CollisionPolicySnapshot DecodePolicy(const CanonicalValue &value) {
  ExactObject(value, 7U, "collision policy");
  core::CollisionPolicySnapshot policy;
  policy.policy_id = String(Member(value, "policy_id"), "policy_id");
  const auto &revision = Member(value, "revision");
  ExactObject(revision, 2U, "policy revision");
  policy.revision = {
      DecodeUnsigned(Member(revision, "revision_id"), "policy revision_id"),
      String(Member(revision, "content_hash"), "policy content_hash"),
  };
  policy.allowed_static_static_pairs =
      DecodePairs(Member(value, "allowed_static_static_pairs"));
  policy.allowed_touch_link_pairs =
      DecodePairs(Member(value, "allowed_touch_link_pairs"));
  for (const auto &entry :
       Array(Member(value, "support_contacts"), "support_contacts")) {
    ExactObject(entry, 11U, "support contact");
    core::SupportContactPolicyEntry contact;
    contact.pair = DecodePair(Member(entry, "pair"));
    contact.mode = DecodeContactMode(
        String(Member(entry, "mode"), "support contact mode"));
    contact.release_evidence_mode = DecodeReleaseMode(String(
        Member(entry, "release_evidence_mode"), "release evidence mode"));
    contact.enforce_max_start_penetration =
        Boolean(Member(entry, "enforce_max_start_penetration"),
                "enforce_max_start_penetration");
    contact.max_start_penetration_m = Number(
        Member(entry, "max_start_penetration_m"), "max_start_penetration_m");
    contact.release_tolerance_m =
        Number(Member(entry, "release_tolerance_m"), "release_tolerance_m");
    contact.min_release_progress_m = Number(
        Member(entry, "min_release_progress_m"), "min_release_progress_m");
    contact.max_contact_penetration_m =
        Number(Member(entry, "max_contact_penetration_m"),
               "max_contact_penetration_m");
    contact.require_separation_by_end =
        Boolean(Member(entry, "require_separation_by_end"),
                "require_separation_by_end");
    contact.require_strictly_decreasing_penetration =
        Boolean(Member(entry, "require_strictly_decreasing_penetration"),
                "require_strictly_decreasing_penetration");
    contact.allow_recontact =
        Boolean(Member(entry, "allow_recontact"), "allow_recontact");
    policy.support_contacts.emplace_back(std::move(contact));
  }
  for (const auto &entry :
       Array(Member(value, "contact_grasps"), "contact_grasps")) {
    ExactObject(entry, 5U, "contact grasp");
    core::ContactGraspPolicyEntry grasp;
    grasp.policy_id = String(Member(entry, "policy_id"), "grasp policy_id");
    grasp.selected_object_id =
        String(Member(entry, "selected_object_id"), "selected_object_id");
    for (const auto &link :
         Array(Member(entry, "allowed_robot_links"), "allowed_robot_links")) {
      grasp.allowed_robot_links.emplace_back(String(link, "robot link"));
    }
    grasp.max_penetration_m =
        Number(Member(entry, "max_penetration_m"), "max_penetration_m");
    grasp.terminal_window_start_fraction =
        Number(Member(entry, "terminal_window_start_fraction"),
               "terminal_window_start_fraction");
    policy.contact_grasps.emplace_back(std::move(grasp));
  }
  policy.require_full_diagnostics = Boolean(
      Member(value, "require_full_diagnostics"), "require_full_diagnostics");
  return policy;
}

CanonicalValue Frame(const core::SceneFrameRefDTO &frame) {
  return std::visit(
      [](const auto &value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::CellSceneFrameRefDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"cell_id", CanonicalValue(value.cell_id.value())},
              {"kind", CanonicalValue("cell_frame")},
          });
        } else if constexpr (std::is_same_v<T,
                                            core::ComponentSceneFrameRefDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"controllable_component_id",
               CanonicalValue(value.controllable_component_id.value())},
              {"frame_role_id", CanonicalValue(value.frame_role_id.value())},
              {"kind", CanonicalValue("component_frame")},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"frame_role_id", CanonicalValue(value.frame_role_id.value())},
              {"kind", CanonicalValue("passive_model_frame")},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
          });
        }
      },
      frame);
}

core::SceneFrameRefDTO DecodeFrame(const CanonicalValue &value) {
  const auto kind = String(Member(value, "kind"), "frame kind");
  if (kind == "cell_frame") {
    ExactObject(value, 2U, "cell frame");
    return core::CellSceneFrameRefDTO{
        core::CellId(String(Member(value, "cell_id"), "cell_id"))};
  }
  if (kind == "component_frame") {
    ExactObject(value, 4U, "component frame");
    return core::ComponentSceneFrameRefDTO{
        core::ControllableComponentId(
            String(Member(value, "controllable_component_id"),
                   "controllable_component_id")),
        core::ModelInstanceId(
            String(Member(value, "model_instance_id"), "model_instance_id")),
        core::FrameRoleId(
            String(Member(value, "frame_role_id"), "frame_role_id")),
    };
  }
  if (kind == "passive_model_frame") {
    ExactObject(value, 3U, "passive model frame");
    return core::PassiveModelSceneFrameRefDTO{
        core::ModelInstanceId(
            String(Member(value, "model_instance_id"), "model_instance_id")),
        core::FrameRoleId(
            String(Member(value, "frame_role_id"), "frame_role_id")),
    };
  }
  Fail("scene frame kind is unsupported");
}

CanonicalValue RequestFrame(const core::SceneTransactionFrameRefDTO &frame) {
  return std::visit(
      [](const auto &value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<
                          T, core::SceneResourceSceneFrameRefDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("scene_resource")},
              {"scene_resource_id",
               CanonicalValue(value.scene_resource_id.value())},
          });
        } else {
          return Frame(core::SceneFrameRefDTO{value});
        }
      },
      frame);
}

core::SceneTransactionFrameRefDTO
DecodeRequestFrame(const CanonicalValue &value) {
  if (String(Member(value, "kind"), "frame kind") == "scene_resource") {
    ExactObject(value, 2U, "scene-resource frame");
    return core::SceneResourceSceneFrameRefDTO{
        core::SceneResourceId(String(Member(value, "scene_resource_id"),
                                     "scene_resource_id"))};
  }
  return core::ToSceneTransactionFrameRef(DecodeFrame(value));
}

CanonicalValue
ProviderEvidence(const core::SceneObjectProviderEvidenceDTO &value) {
  CanonicalValue::Object object{
      {"provider_instance_id",
       CanonicalValue(value.provider_instance_id.value())},
      {"provider_manifest_id",
       CanonicalValue(value.provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(value.provider_manifest_revision.value())},
      {"provider_source_epoch", CanonicalValue(value.provider_source_epoch)},
  };
  return CanonicalValue(std::move(object));
}

core::SceneObjectProviderEvidenceDTO
DecodeProviderEvidence(const CanonicalValue &value) {
  ExactObject(value, 4U, "provider evidence");
  return core::SceneObjectProviderEvidenceDTO{
      core::ProviderInstanceId(String(Member(value, "provider_instance_id"),
                                      "provider_instance_id")),
      core::ProviderManifestId(String(Member(value, "provider_manifest_id"),
                                      "provider_manifest_id")),
      core::ProviderManifestRevision(
          String(Member(value, "provider_manifest_revision"),
                 "provider_manifest_revision")),
      String(Member(value, "provider_source_epoch"), "provider_source_epoch"),
  };
}

// Observation fields that v9 stores kept in provider evidence.
constexpr std::string_view kRetiredProviderEvidenceFields[] = {
    "provider_source_sequence", "received_at_ns", "source_freshness_budget_ns",
    "source_timestamp_ns"};

CanonicalValue WithoutRetiredProviderEvidenceFields(
    const CanonicalValue &value, bool &changed,
    const bool provider_evidence = false) {
  if (value.is_array()) {
    CanonicalValue::Array result;
    for (const auto &item : value.as_array()) {
      result.push_back(WithoutRetiredProviderEvidenceFields(item, changed));
    }
    return CanonicalValue(std::move(result));
  }
  if (!value.is_object()) {
    return value;
  }
  CanonicalValue::Object result;
  for (const auto &[name, member] : value.as_object()) {
    if (provider_evidence &&
        std::find(std::begin(kRetiredProviderEvidenceFields),
                  std::end(kRetiredProviderEvidenceFields),
                  name) != std::end(kRetiredProviderEvidenceFields)) {
      changed = true;
      continue;
    }
    result.emplace_back(name, WithoutRetiredProviderEvidenceFields(
                                  member, changed,
                                  name == "provider_evidence"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue Placement(const core::SceneObjectPlacementDTO &placement) {
  return std::visit(
      [](const auto &value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,
                                     core::CellFixedSceneObjectPlacementDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"cell_id", CanonicalValue(value.cell_id.value())},
              {"kind", CanonicalValue("cell_fixed")},
              {"pose_in_cell", Pose(value.pose_in_cell)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"carried_object_attachment_id",
               CanonicalValue(value.carried_object_attachment_id.value())},
              {"carried_object_attachment_revision",
               CanonicalValue(
                   value.carried_object_attachment_revision.value())},
              {"kind", CanonicalValue("carried")},
          });
        }
      },
      placement);
}

core::SceneObjectPlacementDTO DecodePlacement(const CanonicalValue &value) {
  const auto kind = String(Member(value, "kind"), "placement kind");
  if (kind == "cell_fixed") {
    ExactObject(value, 3U, "cell-fixed placement");
    return core::CellFixedSceneObjectPlacementDTO{
        core::CellId(String(Member(value, "cell_id"), "cell_id")),
        DecodePose(Member(value, "pose_in_cell")),
    };
  }
  if (kind == "carried") {
    ExactObject(value, 3U, "carried placement");
    return core::CarriedSceneObjectPlacementDTO{
        core::CarriedObjectAttachmentId(
            String(Member(value, "carried_object_attachment_id"),
                   "carried_object_attachment_id")),
        core::CarriedObjectAttachmentRevision(
            String(Member(value, "carried_object_attachment_revision"),
                   "carried_object_attachment_revision")),
    };
  }
  Fail("scene placement kind is unsupported");
}

CanonicalValue Object(const core::SceneObjectSnapshotDTO &object) {
  CanonicalValue::Array shapes;
  shapes.reserve(object.collision_shapes.size());
  for (const auto &shape : object.collision_shapes) {
    shapes.emplace_back(Shape(shape));
  }
  CanonicalValue::Object value{
      {"collision_shapes", CanonicalValue(std::move(shapes))},
      {"placement", Placement(object.placement)},
      {"scene_resource_id", CanonicalValue(object.scene_resource_id.value())},
      {"support_contacts", SupportDeclarations(object.support_contacts)},
  };
  if (object.provider_evidence.has_value()) {
    value.emplace_back("provider_evidence",
                       ProviderEvidence(*object.provider_evidence));
  }
  return CanonicalValue(std::move(value));
}

core::SceneObjectSnapshotDTO DecodeObject(const CanonicalValue &value) {
  if (!value.is_object() || value.as_object().size() < 4U ||
      value.as_object().size() > 5U) {
    Fail("scene object has an unknown or missing field");
  }
  core::SceneObjectSnapshotDTO result{
      core::SceneResourceId(
          String(Member(value, "scene_resource_id"), "scene_resource_id")),
      {},
      DecodePlacement(Member(value, "placement")),
      std::nullopt,
      DecodeSupportDeclarations(Member(value, "support_contacts")),
  };
  for (const auto &shape :
       Array(Member(value, "collision_shapes"), "collision_shapes")) {
    result.collision_shapes.emplace_back(DecodeShape(shape));
  }
  if (const auto *evidence = value.find_object_member("provider_evidence")) {
    result.provider_evidence = DecodeProviderEvidence(*evidence);
  }
  return result;
}

CanonicalValue
GraspLostBound(const core::CarriedObjectGraspLostBoundDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"rotation_rad", CanonicalValue(value.rotation_rad)},
      {"translation_m", CanonicalValue(value.translation_m)},
  });
}

core::CarriedObjectGraspLostBoundDTO
DecodeGraspLostBound(const CanonicalValue &value) {
  // Positivity is the scene owner's attach rule; a durably recorded
  // rejected request must still decode.
  ExactObject(value, 2U, "grasp-lost bound");
  return {Number(Member(value, "translation_m"), "grasp-lost translation_m"),
          Number(Member(value, "rotation_rad"), "grasp-lost rotation_rad")};
}

CanonicalValue Attachment(const core::CarriedObjectAttachmentDTO &value) {
  CanonicalValue::Object result{
      {"carried_object_attachment_id",
       CanonicalValue(value.carried_object_attachment_id.value())},
      {"carried_object_attachment_revision",
       CanonicalValue(value.carried_object_attachment_revision.value())},
      {"carrier_frame_ref", Frame(value.carrier_frame_ref)},
      {"command_target_id", CanonicalValue(value.command_target_id.value())},
      {"controllable_component_id",
       CanonicalValue(value.controllable_component_id.value())},
      {"pose_in_carrier_frame", Pose(value.pose_in_carrier_frame)},
      {"scene_resource_id", CanonicalValue(value.scene_resource_id.value())},
      {"touch_body_roles", StrongArray(value.touch_body_roles)},
  };
  // Absent for attachments without a bound, so older rows encode unchanged.
  if (value.grasp_lost_check) {
    result.emplace_back(
        "grasp_lost_check",
        CanonicalValue(CanonicalValue::Object{
            {"bound", GraspLostBound(value.grasp_lost_check->bound)},
        }));
  }
  return CanonicalValue(std::move(result));
}

core::CarriedObjectAttachmentDTO DecodeAttachment(const CanonicalValue &value) {
  const auto *check = value.find_object_member("grasp_lost_check");
  ExactObject(value, check ? 9U : 8U, "carried attachment");
  std::optional<core::CarriedObjectGraspLostCheckDTO> grasp_lost_check;
  if (check) {
    ExactObject(*check, 1U, "grasp-lost check");
    grasp_lost_check = core::CarriedObjectGraspLostCheckDTO{
        DecodeGraspLostBound(Member(*check, "bound"))};
  }
  return {
      core::CarriedObjectAttachmentId(
          String(Member(value, "carried_object_attachment_id"),
                 "carried_object_attachment_id")),
      core::CarriedObjectAttachmentRevision(
          String(Member(value, "carried_object_attachment_revision"),
                 "carried_object_attachment_revision")),
      core::SceneResourceId(
          String(Member(value, "scene_resource_id"), "scene_resource_id")),
      core::ControllableComponentId(
          String(Member(value, "controllable_component_id"),
                 "controllable_component_id")),
      core::CommandTargetId(
          String(Member(value, "command_target_id"), "command_target_id")),
      DecodeFrame(Member(value, "carrier_frame_ref")),
      DecodePose(Member(value, "pose_in_carrier_frame")),
      DecodeStrongArray<core::CollisionBodyRoleId>(
          Member(value, "touch_body_roles"), "touch_body_roles"),
      std::move(grasp_lost_check),
  };
}

CanonicalValue Projection(const core::ProjectionRevisionDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"scene_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(value.scene_revision.content_hash.value())},
           {"revision_id",
            Unsigned(value.scene_revision.revision_id, "scene revision")},
           {"scene_id", CanonicalValue(value.scene_revision.scene_id.value())},
       })},
  });
}

core::ProjectionRevisionDTO DecodeProjection(const CanonicalValue &value) {
  ExactObject(value, 3U, "projection revision");
  const auto &scene = Member(value, "scene_revision");
  ExactObject(scene, 3U, "scene revision");
  core::ProjectionRevisionDTO projection{
      core::RetainedBundleId(
          String(Member(value, "retained_bundle_id"), "retained_bundle_id")),
      core::GraphRevision(
          String(Member(value, "graph_revision"), "graph_revision")),
      core::SceneRevision{
          core::SceneId(String(Member(scene, "scene_id"), "scene_id")),
          DecodeUnsigned(Member(scene, "revision_id"), "revision_id"),
          core::ContentDigest(
              String(Member(scene, "content_hash"), "content_hash")),
      },
  };
  if (!projection.valid()) {
    Fail("projection revision is invalid");
  }
  return projection;
}

CanonicalValue RequestContext(const core::PublicRequestContextDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"correlation_id", value.correlation_id.has_value()
                             ? CanonicalValue(*value.correlation_id)
                             : CanonicalValue(nullptr)},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"request_owner_epoch",
       CanonicalValue(value.request_owner_epoch.value())},
      {"source_label", CanonicalValue(value.source_label)},
  });
}

core::PublicRequestContextDTO
DecodeRequestContext(const CanonicalValue &value) {
  ExactObject(value, 4U, "request context");
  std::optional<std::string> correlation_id;
  const auto &correlation = Member(value, "correlation_id");
  if (!std::holds_alternative<std::nullptr_t>(correlation.storage())) {
    correlation_id = String(correlation, "correlation_id");
  }
  return {
      core::RequestId(String(Member(value, "request_id"), "request_id")),
      core::RequestOwnerEpoch(
          String(Member(value, "request_owner_epoch"), "request_owner_epoch")),
      String(Member(value, "source_label"), "source_label"),
      std::move(correlation_id),
  };
}

std::string_view Transport(const core::RequestTransportDTO value) {
  switch (value) {
  case core::RequestTransportDTO::kRos:
    return "ros";
  case core::RequestTransportDTO::kHttp:
    return "http";
  case core::RequestTransportDTO::kInternal:
    return "internal";
  }
  return {};
}

std::string_view Assurance(const core::RequestAssuranceDTO value) {
  switch (value) {
  case core::RequestAssuranceDTO::kUnverified:
    return "unverified";
  case core::RequestAssuranceDTO::kExternallyAuthenticated:
    return "externally_authenticated";
  }
  return {};
}

CanonicalValue Attribution(const core::RequestAttributionDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"assurance", CanonicalValue(std::string(Assurance(value.assurance)))},
      {"principal_id", value.principal_id.has_value()
                           ? CanonicalValue(*value.principal_id)
                           : CanonicalValue(nullptr)},
      {"transport", CanonicalValue(std::string(Transport(value.transport)))},
  });
}

core::RequestAttributionDTO DecodeAttribution(const CanonicalValue &value) {
  ExactObject(value, 3U, "request attribution");
  const auto transport_text = String(Member(value, "transport"), "transport");
  const auto assurance_text = String(Member(value, "assurance"), "assurance");
  core::RequestTransportDTO transport;
  if (transport_text == "ros") {
    transport = core::RequestTransportDTO::kRos;
  } else if (transport_text == "http") {
    transport = core::RequestTransportDTO::kHttp;
  } else if (transport_text == "internal") {
    transport = core::RequestTransportDTO::kInternal;
  } else {
    Fail("request attribution transport is invalid");
  }
  core::RequestAssuranceDTO assurance;
  if (assurance_text == "unverified") {
    assurance = core::RequestAssuranceDTO::kUnverified;
  } else if (assurance_text == "externally_authenticated") {
    assurance = core::RequestAssuranceDTO::kExternallyAuthenticated;
  } else {
    Fail("request attribution assurance is invalid");
  }
  std::optional<std::string> principal_id;
  const auto &principal = Member(value, "principal_id");
  if (!std::holds_alternative<std::nullptr_t>(principal.storage())) {
    principal_id = String(principal, "principal_id");
  }
  core::RequestAttributionDTO result{
      transport,
      assurance,
      std::move(principal_id),
  };
  if (!result.locally_valid()) {
    Fail("request attribution is invalid");
  }
  return result;
}

CanonicalValue RequestOperation(const core::SceneOperationRequestDTO &value) {
  return std::visit(
      [](const auto &request) -> CanonicalValue {
        using T = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<T, core::AddObjectRequestDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"frame_ref", RequestFrame(request.frame_ref)},
              {"kind", CanonicalValue("add_object")},
              {"pose", Pose(request.pose)},
              {"scene_resource_id",
               CanonicalValue(request.scene_resource_id.value())},
              {"shape", Shape(request.shape)},
              {"support_contacts",
               SupportDeclarations(request.support_contacts)},
          });
        } else if constexpr (std::is_same_v<T, core::UpsertObjectRequestDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"frame_ref", RequestFrame(request.frame_ref)},
              {"kind", CanonicalValue("upsert_object")},
              {"pose", Pose(request.pose)},
              {"scene_resource_id",
               CanonicalValue(request.scene_resource_id.value())},
              {"shape", Shape(request.shape)},
              {"support_contacts",
               SupportDeclarations(request.support_contacts)},
          });
        } else if constexpr (std::is_same_v<T, core::RemoveObjectRequestDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("remove_object")},
              {"scene_resource_id",
               CanonicalValue(request.scene_resource_id.value())},
          });
        } else if constexpr (std::is_same_v<T,
                                            core::UpdateObjectPoseRequestDTO>) {
          CanonicalValue::Object object{
              {"expected_attached",
               request.expected_attached.has_value()
                   ? CanonicalValue(*request.expected_attached)
                   : CanonicalValue(nullptr)},
              {"frame_ref", RequestFrame(request.frame_ref)},
              {"kind", CanonicalValue("update_object_pose")},
              {"pose", Pose(request.pose)},
              {"provider_evidence",
               request.provider_evidence.has_value()
                   ? ProviderEvidence(*request.provider_evidence)
                   : CanonicalValue(nullptr)},
              {"scene_resource_id",
               CanonicalValue(request.scene_resource_id.value())},
          };
          return CanonicalValue(std::move(object));
        } else if constexpr (std::is_same_v<
                                 T, core::AttachCarriedObjectRequestDTO>) {
          CanonicalValue::Object object{
              {"carried_object_attachment_id",
               CanonicalValue(request.carried_object_attachment_id.value())},
              {"command_target_id",
               CanonicalValue(request.command_target_id.value())},
              {"controllable_component_id",
               CanonicalValue(request.controllable_component_id.value())},
              {"expected_component_attachment_revisions",
               StrongArray(request.expected_component_attachment_revisions)},
              {"frame_role_id", CanonicalValue(request.frame_role_id.value())},
              {"kind", CanonicalValue("attach_carried_object")},
              {"scene_resource_id",
               CanonicalValue(request.scene_resource_id.value())},
              {"touch_body_roles", StrongArray(request.touch_body_roles)},
          };
          if (request.grasp_lost_bound) {
            object.emplace_back("grasp_lost_bound",
                                GraspLostBound(*request.grasp_lost_bound));
          }
          return CanonicalValue(std::move(object));
        } else if constexpr (std::is_same_v<
                                 T, core::DetachCarriedObjectRequestDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"carried_object_attachment_id",
               CanonicalValue(request.carried_object_attachment_id.value())},
              {"expected_carried_object_attachment_revision",
               CanonicalValue(
                   request.expected_carried_object_attachment_revision
                       .value())},
              {"frame_ref", RequestFrame(request.frame_ref)},
              {"kind", CanonicalValue("detach_carried_object")},
              {"pose", Pose(request.pose)},
          });
        } else if constexpr (
            std::is_same_v<T,
                           core::DetachCarriedObjectAtCurrentPoseRequestDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"carried_object_attachment_id",
               CanonicalValue(request.carried_object_attachment_id.value())},
              {"expected_carried_object_attachment_revision",
               CanonicalValue(
                   request.expected_carried_object_attachment_revision
                       .value())},
              {"expected_command_target_id",
               CanonicalValue(request.expected_command_target_id.value())},
              {"expected_controllable_component_id",
               CanonicalValue(
                   request.expected_controllable_component_id.value())},
              {"expected_scene_resource_id",
               CanonicalValue(request.expected_scene_resource_id.value())},
              {"kind", CanonicalValue("detach_carried_object_at_current_pose")},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"configured_policy", Policy(request.configured_policy)},
              {"expected_configured_policy_revision",
               CanonicalValue(CanonicalValue::Object{
                   {"content_hash",
                    CanonicalValue(request.expected_configured_policy_revision
                                       .content_hash)},
                   {"revision_id",
                    Unsigned(
                        request.expected_configured_policy_revision.revision_id,
                        "expected policy revision")},
               })},
              {"kind", CanonicalValue("set_collision_policy")},
          });
        }
      },
      value);
}

core::SceneOperationRequestDTO
DecodeRequestOperation(const CanonicalValue &value) {
  if (!value.is_object()) {
    Fail("scene operation must be an object");
  }
  const auto kind = String(Member(value, "kind"), "operation kind");
  if (kind == "add_object") {
    ExactObject(value, 6U, "add-object request");
    return core::AddObjectRequestDTO{
        core::SceneResourceId(
            String(Member(value, "scene_resource_id"), "scene_resource_id")),
        DecodeShape(Member(value, "shape")),
        DecodePose(Member(value, "pose")),
        DecodeRequestFrame(Member(value, "frame_ref")),
        DecodeSupportDeclarations(Member(value, "support_contacts")),
    };
  }
  if (kind == "upsert_object") {
    ExactObject(value, 6U, "upsert-object request");
    return core::UpsertObjectRequestDTO{
        core::SceneResourceId(
            String(Member(value, "scene_resource_id"), "scene_resource_id")),
        DecodeShape(Member(value, "shape")),
        DecodePose(Member(value, "pose")),
        DecodeRequestFrame(Member(value, "frame_ref")),
        DecodeSupportDeclarations(Member(value, "support_contacts")),
    };
  }
  if (kind == "remove_object") {
    ExactObject(value, 2U, "remove-object request");
    return core::RemoveObjectRequestDTO{
        core::SceneResourceId(
            String(Member(value, "scene_resource_id"), "scene_resource_id")),
    };
  }
  if (kind == "update_object_pose") {
    ExactObject(value, 6U, "update-object-pose request");
    std::optional<core::SceneObjectProviderEvidenceDTO> provider_evidence;
    const auto &provider = Member(value, "provider_evidence");
    if (!std::holds_alternative<std::nullptr_t>(provider.storage())) {
      provider_evidence = DecodeProviderEvidence(provider);
    }
    std::optional<bool> expected_attached;
    const auto &attached = Member(value, "expected_attached");
    if (!std::holds_alternative<std::nullptr_t>(attached.storage())) {
      expected_attached = Boolean(attached, "expected_attached");
    }
    return core::UpdateObjectPoseRequestDTO{
        core::SceneResourceId(
            String(Member(value, "scene_resource_id"), "scene_resource_id")),
        DecodePose(Member(value, "pose")),
        DecodeRequestFrame(Member(value, "frame_ref")),
        std::move(provider_evidence),
        expected_attached,
    };
  }
  if (kind == "attach_carried_object") {
    const auto *bound = value.find_object_member("grasp_lost_bound");
    ExactObject(value, bound ? 9U : 8U, "attach-carried-object request");
    return core::AttachCarriedObjectRequestDTO{
        core::CarriedObjectAttachmentId(
            String(Member(value, "carried_object_attachment_id"),
                   "carried_object_attachment_id")),
        core::SceneResourceId(
            String(Member(value, "scene_resource_id"), "scene_resource_id")),
        core::ControllableComponentId(
            String(Member(value, "controllable_component_id"),
                   "controllable_component_id")),
        core::CommandTargetId(
            String(Member(value, "command_target_id"), "command_target_id")),
        core::FrameRoleId(
            String(Member(value, "frame_role_id"), "frame_role_id")),
        DecodeStrongArray<core::CollisionBodyRoleId>(
            Member(value, "touch_body_roles"), "touch_body_roles"),
        DecodeStrongArray<core::ComponentAttachmentRevision>(
            Member(value, "expected_component_attachment_revisions"),
            "expected_component_attachment_revisions"),
        bound ? std::optional(DecodeGraspLostBound(*bound)) : std::nullopt,
    };
  }
  if (kind == "detach_carried_object") {
    ExactObject(value, 5U, "detach-carried-object request");
    return core::DetachCarriedObjectRequestDTO{
        core::CarriedObjectAttachmentId(
            String(Member(value, "carried_object_attachment_id"),
                   "carried_object_attachment_id")),
        core::CarriedObjectAttachmentRevision(
            String(Member(value, "expected_carried_object_attachment_revision"),
                   "expected_carried_object_attachment_revision")),
        DecodePose(Member(value, "pose")),
        DecodeRequestFrame(Member(value, "frame_ref")),
    };
  }
  if (kind == "detach_carried_object_at_current_pose") {
    ExactObject(value, 6U, "detach-carried-object-at-current-pose request");
    return core::DetachCarriedObjectAtCurrentPoseRequestDTO{
        core::CarriedObjectAttachmentId(
            String(Member(value, "carried_object_attachment_id"),
                   "carried_object_attachment_id")),
        core::CarriedObjectAttachmentRevision(
            String(Member(value, "expected_carried_object_attachment_revision"),
                   "expected_carried_object_attachment_revision")),
        core::SceneResourceId(
            String(Member(value, "expected_scene_resource_id"),
                   "expected_scene_resource_id")),
        core::ControllableComponentId(
            String(Member(value, "expected_controllable_component_id"),
                   "expected_controllable_component_id")),
        core::CommandTargetId(
            String(Member(value, "expected_command_target_id"),
                   "expected_command_target_id")),
    };
  }
  if (kind == "set_collision_policy") {
    ExactObject(value, 3U, "set-collision-policy request");
    const auto &expected = Member(value, "expected_configured_policy_revision");
    ExactObject(expected, 2U, "expected collision-policy revision");
    return core::SetCollisionPolicyRequestDTO{
        DecodePolicy(Member(value, "configured_policy")),
        core::CollisionPolicyRevision{
            DecodeUnsigned(Member(expected, "revision_id"),
                           "expected policy revision_id"),
            String(Member(expected, "content_hash"),
                   "expected policy content_hash"),
        },
    };
  }
  Fail("scene operation kind is unsupported");
}

CanonicalValue ResolvedOperation(const core::ResolvedSceneOperationDTO &value) {
  return std::visit(
      [](const auto &operation) -> CanonicalValue {
        using T = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<T, core::ResolvedUpsertSceneObjectDTO>) {
          CanonicalValue::Object result{
              {"kind", CanonicalValue("upsert_object")},
              {"object", Object(operation.object)}};
          if (operation.attachment)
            result.emplace_back("attachment",
                                Attachment(*operation.attachment));
          return CanonicalValue(std::move(result));
        } else if constexpr (std::is_same_v<
                                 T, core::ResolvedRemoveSceneObjectDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("remove_object")},
              {"scene_resource_id",
               CanonicalValue(operation.scene_resource_id.value())},
          });
        } else if constexpr (std::is_same_v<
                                 T, core::ResolvedAttachCarriedObjectDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"attachment", Attachment(operation.attachment)},
              {"kind", CanonicalValue("attach_carried_object")},
              {"object", Object(operation.object)},
          });
        } else if constexpr (std::is_same_v<
                                 T, core::ResolvedDetachCarriedObjectDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("detach_carried_object")},
              {"object", Object(operation.object)},
              {"removed_attachment_id",
               CanonicalValue(operation.removed_attachment_id.value())},
              {"removed_attachment_revision",
               CanonicalValue(operation.removed_attachment_revision.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"configured_policy", Policy(operation.configured_policy)},
              {"effective_policy", Policy(operation.effective_policy)},
              {"kind", CanonicalValue("set_collision_policy")},
          });
        }
      },
      value);
}

core::ResolvedSceneOperationDTO
DecodeResolvedOperation(const CanonicalValue &value) {
  const auto kind = String(Member(value, "kind"), "resolved operation kind");
  if (kind == "upsert_object") {
    const auto *attachment = value.find_object_member("attachment");
    ExactObject(value, attachment ? 3U : 2U,
                "resolved upsert-object operation");
    return core::ResolvedUpsertSceneObjectDTO{
        DecodeObject(Member(value, "object")),
        attachment ? std::optional(DecodeAttachment(*attachment))
                   : std::nullopt};
  }
  if (kind == "remove_object") {
    ExactObject(value, 2U, "resolved remove-object operation");
    return core::ResolvedRemoveSceneObjectDTO{core::SceneResourceId(
        String(Member(value, "scene_resource_id"), "scene_resource_id"))};
  }
  if (kind == "attach_carried_object") {
    ExactObject(value, 3U, "resolved attach-carried-object operation");
    return core::ResolvedAttachCarriedObjectDTO{
        DecodeObject(Member(value, "object")),
        DecodeAttachment(Member(value, "attachment"))};
  }
  if (kind == "detach_carried_object") {
    ExactObject(value, 4U, "resolved detach-carried-object operation");
    return core::ResolvedDetachCarriedObjectDTO{
        DecodeObject(Member(value, "object")),
        core::CarriedObjectAttachmentId(String(
            Member(value, "removed_attachment_id"),
            "removed_attachment_id")),
        core::CarriedObjectAttachmentRevision(String(
            Member(value, "removed_attachment_revision"),
            "removed_attachment_revision"))};
  }
  if (kind == "set_collision_policy") {
    ExactObject(value, 3U, "resolved collision-policy operation");
    return core::ResolvedCollisionPolicyDTO{
        DecodePolicy(Member(value, "configured_policy")),
        DecodePolicy(Member(value, "effective_policy"))};
  }
  Fail("resolved scene operation kind is unsupported");
}

CanonicalValue ResolvedFrame(const core::ResolvedFrameDTO &value) {
  return std::visit(
      [](const auto &frame) -> CanonicalValue {
        using T = std::decay_t<decltype(frame)>;
        if constexpr (std::is_same_v<T, core::ResolvedCellFrameDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"cell_id", CanonicalValue(frame.cell_id.value())},
              {"compiled_frame_name",
               CanonicalValue(frame.compiled_frame_name)},
              {"kind", CanonicalValue("cell")},
          });
        } else if constexpr (std::is_same_v<
                                 T, core::ResolvedComponentModelFrameDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"compiled_frame_name",
               CanonicalValue(frame.compiled_frame_name)},
              {"controllable_component_id",
               CanonicalValue(frame.controllable_component_id.value())},
              {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
              {"kind", CanonicalValue("component")},
              {"model_instance_id",
               CanonicalValue(frame.model_instance_id.value())},
              {"model_revision",
               CanonicalValue(frame.model_revision.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"compiled_frame_name",
               CanonicalValue(frame.compiled_frame_name)},
              {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
              {"kind", CanonicalValue("passive")},
              {"model_instance_id",
               CanonicalValue(frame.model_instance_id.value())},
              {"model_revision",
               CanonicalValue(frame.model_revision.value())},
          });
        }
      },
      value);
}

core::ResolvedFrameDTO DecodeResolvedFrame(const CanonicalValue &value) {
  const auto kind = String(Member(value, "kind"), "resolved frame kind");
  if (kind == "cell") {
    ExactObject(value, 3U, "resolved cell frame");
    return core::ResolvedCellFrameDTO{
        core::CellId(String(Member(value, "cell_id"), "cell_id")),
        String(Member(value, "compiled_frame_name"),
               "compiled_frame_name")};
  }
  if (kind == "component") {
    ExactObject(value, 6U, "resolved component frame");
    return core::ResolvedComponentModelFrameDTO{
        core::ControllableComponentId(String(
            Member(value, "controllable_component_id"),
            "controllable_component_id")),
        core::ModelInstanceId(String(Member(value, "model_instance_id"),
                                     "model_instance_id")),
        core::FrameRoleId(
            String(Member(value, "frame_role_id"), "frame_role_id")),
        String(Member(value, "compiled_frame_name"),
               "compiled_frame_name"),
        core::ModelRevision(
            String(Member(value, "model_revision"), "model_revision"))};
  }
  if (kind == "passive") {
    ExactObject(value, 5U, "resolved passive frame");
    return core::ResolvedPassiveModelFrameDTO{
        core::ModelInstanceId(String(Member(value, "model_instance_id"),
                                     "model_instance_id")),
        core::FrameRoleId(
            String(Member(value, "frame_role_id"), "frame_role_id")),
        String(Member(value, "compiled_frame_name"),
               "compiled_frame_name"),
        core::ModelRevision(
            String(Member(value, "model_revision"), "model_revision"))};
  }
  Fail("resolved frame kind is unsupported");
}

CanonicalValue
ProjectionLeaseBinding(const core::ProjectionLeaseBindingDTO &value) {
  if (!value.valid()) {
    Fail("cannot encode an invalid projection lease binding");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"projection_lease_id",
       CanonicalValue(value.projection_lease_id.value())},
      {"projection_revision", Projection(value.projection_revision)},
  });
}

core::ProjectionLeaseBindingDTO
DecodeProjectionLeaseBinding(const CanonicalValue &value) {
  ExactObject(value, 2U, "projection lease binding");
  core::ProjectionLeaseBindingDTO result{
      DecodeProjection(Member(value, "projection_revision")),
      core::ProjectionLeaseId(String(Member(value, "projection_lease_id"),
                                     "projection_lease_id"))};
  if (!result.valid()) {
    Fail("decoded projection lease binding is invalid");
  }
  return result;
}

CanonicalValue SceneFrameResolutionReceipt(
    const core::SceneFrameResolutionReceiptDTO &value) {
  if (!value.valid()) {
    Fail("cannot encode an invalid scene-frame resolution receipt");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"component_attachment_revisions",
       StrongArray(value.component_attachment_revisions)},
      {"frame_pose_in_cell", Pose(value.frame_pose_in_cell)},
      {"predicted_state_digest",
       value.predicted_state_digest.has_value()
           ? CanonicalValue(value.predicted_state_digest->value())
           : CanonicalValue(nullptr)},
      {"projection_binding", ProjectionLeaseBinding(value.projection_binding)},
      {"resolution_evidence_digest",
       CanonicalValue(value.resolution_evidence_digest.value())},
      {"resolved_frame", ResolvedFrame(value.resolved_frame)},
  });
}

core::SceneFrameResolutionReceiptDTO
DecodeSceneFrameResolutionReceipt(const CanonicalValue &value) {
  ExactObject(value, 6U, "scene-frame resolution receipt");
  std::optional<core::ContentDigest> predicted_state_digest;
  const auto &predicted = Member(value, "predicted_state_digest");
  if (!std::holds_alternative<std::nullptr_t>(predicted.storage())) {
    predicted_state_digest = core::ContentDigest(
        String(predicted, "predicted_state_digest"));
  }
  core::SceneFrameResolutionReceiptDTO result{
      DecodeResolvedFrame(Member(value, "resolved_frame")),
      DecodePose(Member(value, "frame_pose_in_cell")),
      DecodeStrongArray<core::ComponentAttachmentRevision>(
          Member(value, "component_attachment_revisions"),
          "component_attachment_revisions"),
      DecodeProjectionLeaseBinding(Member(value, "projection_binding")),
      std::move(predicted_state_digest),
      core::ContentDigest(String(Member(value, "resolution_evidence_digest"),
                                 "resolution_evidence_digest"))};
  if (!result.valid()) {
    Fail("decoded scene-frame resolution receipt is invalid");
  }
  return result;
}

core::SceneUpdateStatusDTO DecodeSceneUpdateStatus(
    const CanonicalValue &value) {
  const auto status = String(value, "scene update status");
  if (status == "geometry_changed") {
    return core::SceneUpdateStatusDTO::kGeometryChanged;
  }
  if (status == "freshness_only") {
    return core::SceneUpdateStatusDTO::kFreshnessOnly;
  }
  if (status == "no_op") {
    return core::SceneUpdateStatusDTO::kNoOp;
  }
  Fail("scene update status is unsupported");
}

CanonicalValue Snapshot(const core::LogicalSceneSnapshotDTO &snapshot) {
  if (!snapshot.locally_valid()) {
    Fail("cannot encode an invalid logical snapshot");
  }
  CanonicalValue::Array objects;
  objects.reserve(snapshot.objects.size());
  for (const auto &object : snapshot.objects) {
    objects.emplace_back(Object(object));
  }
  CanonicalValue::Array attachments;
  attachments.reserve(snapshot.carried_object_attachments.size());
  for (const auto &attachment : snapshot.carried_object_attachments) {
    attachments.emplace_back(Attachment(attachment));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"carried_object_attachments", CanonicalValue(std::move(attachments))},
      {"cell_id", CanonicalValue(snapshot.cell_id.value())},
      {"configured_collision_policy",
       Policy(snapshot.configured_collision_policy)},
      {"objects", CanonicalValue(std::move(objects))},
      {"policy_epoch", Unsigned(snapshot.policy_epoch, "policy_epoch")},
      {"revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(snapshot.revision.content_hash.value())},
           {"revision_id",
            Unsigned(snapshot.revision.revision_id, "scene revision")},
           {"scene_id", CanonicalValue(snapshot.revision.scene_id.value())},
       })},
      {"topology_epoch", Unsigned(snapshot.topology_epoch, "topology_epoch")},
      {"transaction_sequence",
       Unsigned(snapshot.transaction_sequence, "transaction_sequence")},
  });
}

CanonicalValue ProgramResetSceneAuthorization(
    const core::ProgramResetSceneAuthorizationDTO &authorization) {
  if (!core::ValidateProgramResetSceneAuthorization(authorization).ok()) {
    Fail("cannot encode an invalid Program Reset scene authorization");
  }
  if (authorization.reset_claim) {
    const auto& reset = *authorization.reset_claim;
    return CanonicalValue(CanonicalValue::Object{
        {"program_runner_owner_epoch", CanonicalValue(authorization.program_runner_owner_epoch.value())},
        {"cycle_generation", Unsigned(reset.cycle_generation, "cycle_generation")},
        {"reset_claim", CanonicalValue(CanonicalValue::Object{
            {"request_owner_epoch", CanonicalValue(reset.request_owner_epoch.value())},
            {"request_id", CanonicalValue(reset.request_id.value())},
            {"request_payload_digest", CanonicalValue(reset.request_payload_digest.value())},
            {"reset_claim_generation", Unsigned(reset.reset_claim_generation.value(), "reset_claim_generation")}})},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"cycle_claim_generation",
       Unsigned(authorization.cycle_claim_generation.value(),
                "cycle_claim_generation")},
      {"program_run_id", CanonicalValue(authorization.program_run_id.value())},
      {"program_runner_owner_epoch",
       CanonicalValue(authorization.program_runner_owner_epoch.value())},
      {"terminal_or_settlement_reference",
       CanonicalValue(
           authorization.terminal_or_settlement_reference.value())},
  });
}

core::ProgramResetSceneAuthorizationDTO DecodeProgramResetSceneAuthorization(
    const CanonicalValue &value) {
  if (value.is_object() && value.find_object_member("reset_claim") != nullptr) {
    ExactObject(value, 3U, "idle Program Reset scene authorization");
    const auto& reset = Member(value, "reset_claim");
    ExactObject(reset, 4U, "idle Program Reset identity");
    core::ProgramResetSceneAuthorizationDTO result{
        core::RequestOwnerEpoch(String(Member(value, "program_runner_owner_epoch"), "program_runner_owner_epoch")),
        core::ProgramRunId(""), core::CellAdmissionClaimGeneration(0U), core::ContentDigest(""),
        core::ProgramResetClaimSceneAuthorizationDTO{
            DecodeUnsigned(Member(value, "cycle_generation"), "cycle_generation"),
            core::RequestOwnerEpoch(String(Member(reset, "request_owner_epoch"), "request_owner_epoch")),
            core::RequestId(String(Member(reset, "request_id"), "request_id")),
            core::ContentDigest(String(Member(reset, "request_payload_digest"), "request_payload_digest")),
            core::CellAdmissionClaimGeneration(DecodeUnsigned(Member(reset, "reset_claim_generation"), "reset_claim_generation"))}};
    if (!core::ValidateProgramResetSceneAuthorization(result).ok()) {
      Fail("idle Program Reset scene authorization is invalid");
    }
    return result;
  }
  ExactObject(value, 4U, "Program Reset scene authorization");
  core::ProgramResetSceneAuthorizationDTO result{
      core::RequestOwnerEpoch(String(Member(value, "program_runner_owner_epoch"),
                                     "program_runner_owner_epoch")),
      core::ProgramRunId(
          String(Member(value, "program_run_id"), "program_run_id")),
      core::CellAdmissionClaimGeneration(DecodeUnsigned(
          Member(value, "cycle_claim_generation"),
          "cycle_claim_generation")),
      core::ContentDigest(String(
          Member(value, "terminal_or_settlement_reference"),
          "terminal_or_settlement_reference")),
  };
  if (!core::ValidateProgramResetSceneAuthorization(result).ok()) {
    Fail("Program Reset scene authorization is invalid");
  }
  return result;
}

CanonicalValue
ProgramNodeEvidence(const core::ProgramNodeAdmissionEvidenceDTO &evidence) {
  return CanonicalValue(CanonicalValue::Object{
      {"program_runner_owner_epoch",
       CanonicalValue(evidence.program_runner_owner_epoch.value())},
      {"program_run_id", CanonicalValue(evidence.program_run_id.value())},
      {"parent_program_cycle_claim_generation",
       CanonicalValue(static_cast<double>(
           evidence.parent_program_cycle_claim_generation.value()))},
      {"node_instance_id", CanonicalValue(evidence.node_instance_id)},
      {"node_attempt_id", CanonicalValue(evidence.node_attempt_id)},
      {"owner_api", CanonicalValue(evidence.owner_api)}});
}

core::ProgramNodeAdmissionEvidenceDTO
DecodeProgramNodeEvidence(const CanonicalValue &value) {
  ExactObject(value, 6U, "Program node evidence");
  return {core::RequestOwnerEpoch(String(
              Member(value, "program_runner_owner_epoch"), "Runner owner")),
          core::ProgramRunId(
              String(Member(value, "program_run_id"), "Program run")),
          core::CellAdmissionClaimGeneration(DecodeUnsigned(
              Member(value, "parent_program_cycle_claim_generation"),
              "parent claim")),
          String(Member(value, "node_instance_id"), "node instance"),
          String(Member(value, "node_attempt_id"), "node attempt"),
          String(Member(value, "owner_api"), "owner API")};
}

} // namespace

std::string EncodeRequest(const core::SceneTransactionRequestDTO &request) {
  CanonicalValue::Object fields{
      {"attribution", Attribution(request.attribution)},
      {"expected_admission_generation",
       request.expected_admission_generation.has_value()
           ? CanonicalValue(static_cast<double>(
                 request.expected_admission_generation->value()))
           : CanonicalValue(nullptr)},
      {"expected_projection_revision",
       Projection(request.expected_projection_revision)},
      {"operation", RequestOperation(request.operation)},
      {"program_reset_scene_authorization",
       request.program_reset_scene_authorization.has_value()
           ? ProgramResetSceneAuthorization(
                 *request.program_reset_scene_authorization)
           : CanonicalValue(nullptr)},
      {"request_context", RequestContext(request.request_context)},
  };
  if (request.program_node_evidence.has_value())
    fields.emplace_back("program_node_evidence",
                        ProgramNodeEvidence(*request.program_node_evidence));
  return ci::canonical_json(CanonicalValue(std::move(fields)));
}

std::string
EncodeRequestDigestInput(const core::SceneTransactionRequestDTO &request) {
  CanonicalValue::Object fields{
      {"expected_projection_revision",
       Projection(request.expected_projection_revision)},
      {"operation", RequestOperation(request.operation)},
      {"program_reset_scene_authorization",
       request.program_reset_scene_authorization.has_value()
           ? ProgramResetSceneAuthorization(
                 *request.program_reset_scene_authorization)
           : CanonicalValue(nullptr)},
      {"request_context", RequestContext(request.request_context)},
  };
  if (request.program_node_evidence.has_value())
    fields.emplace_back("program_node_evidence",
                        ProgramNodeEvidence(*request.program_node_evidence));
  return ci::canonical_json(CanonicalValue(std::move(fields)));
}

core::SceneTransactionRequestDTO DecodeRequest(const std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const auto &root = parsed.root();
  if (ci::canonical_json(root) != bytes) {
    Fail("scene transaction request is not canonical JSON");
  }
  const auto *node_evidence = root.find_object_member("program_node_evidence");
  ExactObject(root, node_evidence == nullptr ? 6U : 7U,
              "scene transaction request");
  std::optional<core::AdmissionGeneration> expected_admission_generation;
  const auto &encoded_admission =
      Member(root, "expected_admission_generation");
  if (!encoded_admission.is_null()) {
    expected_admission_generation = core::AdmissionGeneration(
        DecodeUnsigned(encoded_admission, "expected_admission_generation"));
  }
  std::optional<core::ProgramResetSceneAuthorizationDTO>
      program_reset_scene_authorization;
  const auto &encoded_reset_authorization =
      Member(root, "program_reset_scene_authorization");
  if (!encoded_reset_authorization.is_null()) {
    program_reset_scene_authorization =
        DecodeProgramResetSceneAuthorization(encoded_reset_authorization);
  }
  core::SceneTransactionRequestDTO request{
      DecodeRequestContext(Member(root, "request_context")),
      DecodeAttribution(Member(root, "attribution")),
      DecodeProjection(Member(root, "expected_projection_revision")),
      DecodeRequestOperation(Member(root, "operation")),
      expected_admission_generation,
      program_reset_scene_authorization,
      node_evidence == nullptr
          ? std::nullopt
          : std::optional{DecodeProgramNodeEvidence(*node_evidence)},
  };
  if (!request.attribution.locally_valid() ||
      !request.expected_projection_revision.valid()) {
    Fail("scene transaction request is invalid");
  }
  return request;
}

std::string
EncodeResolvedOperation(const core::ResolvedSceneOperationDTO &operation) {
  return ci::canonical_json(ResolvedOperation(operation));
}

std::string EncodeSnapshot(const core::LogicalSceneSnapshotDTO &snapshot) {
  return ci::canonical_json(Snapshot(snapshot));
}

core::LogicalSceneSnapshotDTO DecodeSnapshot(const std::string_view bytes,
                                             const bool verify_content_hash) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const auto &root = parsed.root();
  if (ci::canonical_json(root) != bytes) {
    Fail("stored snapshot is not canonical JSON");
  }
  ExactObject(root, 8U, "logical scene snapshot");
  const auto &revision = Member(root, "revision");
  ExactObject(revision, 3U, "scene revision");
  core::LogicalSceneSnapshotDTO snapshot{
      core::SceneRevision{
          core::SceneId(String(Member(revision, "scene_id"), "scene_id")),
          DecodeUnsigned(Member(revision, "revision_id"), "revision_id"),
          core::ContentDigest(
              String(Member(revision, "content_hash"), "content_hash")),
      },
      core::CellId(String(Member(root, "cell_id"), "cell_id")),
      DecodeUnsigned(Member(root, "transaction_sequence"),
                     "transaction_sequence"),
      DecodeUnsigned(Member(root, "topology_epoch"), "topology_epoch"),
      DecodeUnsigned(Member(root, "policy_epoch"), "policy_epoch"),
      DecodePolicy(Member(root, "configured_collision_policy")),
      {},
      {},
  };
  for (const auto &object : Array(Member(root, "objects"), "objects")) {
    snapshot.objects.emplace_back(DecodeObject(object));
  }
  for (const auto &attachment :
       Array(Member(root, "carried_object_attachments"),
             "carried_object_attachments")) {
    snapshot.carried_object_attachments.emplace_back(
        DecodeAttachment(attachment));
  }
  if (!snapshot.locally_valid() ||
      (verify_content_hash &&
       core::logical_scene_identity::ComputeLogicalSceneContentHash(
           snapshot) != snapshot.revision.content_hash) ||
      core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
          snapshot.configured_collision_policy)
              .value() !=
          snapshot.configured_collision_policy.revision.content_hash) {
    Fail("stored snapshot identity or local invariants are invalid");
  }
  for (const auto &attachment : snapshot.carried_object_attachments) {
    if (core::logical_scene_identity::ComputeCarriedObjectAttachmentRevision(
            attachment) != attachment.carried_object_attachment_revision) {
      Fail("stored carried attachment revision is invalid");
    }
  }
  return snapshot;
}

std::optional<std::string>
StripRetiredProviderEvidenceFields(const std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  bool changed = false;
  auto stripped =
      WithoutRetiredProviderEvidenceFields(parsed.root(), changed);
  if (!changed) {
    return std::nullopt;
  }
  return ci::canonical_json(stripped);
}

std::string
EncodeErrorDetails(const std::map<std::string, std::string> &details) {
  CanonicalValue::Object object;
  object.reserve(details.size());
  for (const auto &[key, value] : details) {
    object.emplace_back(key, CanonicalValue(value));
  }
  return ci::canonical_json(CanonicalValue(std::move(object)));
}

std::map<std::string, std::string>
DecodeErrorDetails(const std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const auto &root = parsed.root();
  if (!root.is_object() || ci::canonical_json(root) != bytes) {
    Fail("stored error details are not a canonical object");
  }
  std::map<std::string, std::string> result;
  for (const auto &[key, value] : root.as_object()) {
    result.emplace(key, String(value, "error detail"));
  }
  return result;
}

} // namespace manyforge::scene::logical_scene_codec_internal

namespace manyforge::scene {

namespace ci = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using ci::CanonicalValue;

std::string
EncodeLogicalSceneSnapshot(const core::LogicalSceneSnapshotDTO &snapshot) {
  return logical_scene_codec_internal::EncodeSnapshot(snapshot);
}

core::LogicalSceneSnapshotDTO
DecodeLogicalSceneSnapshot(const std::string_view bytes) {
  return logical_scene_codec_internal::DecodeSnapshot(bytes);
}

std::string EncodeResolvedSceneOperation(
    const core::ResolvedSceneOperationDTO &operation) {
  return logical_scene_codec_internal::EncodeResolvedOperation(operation);
}

core::ResolvedSceneOperationDTO
DecodeResolvedSceneOperation(const std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const auto &root = parsed.root();
  if (ci::canonical_json(root) != bytes) {
    throw std::invalid_argument(
        "logical-scene codec: resolved operation is not canonical JSON");
  }
  return logical_scene_codec_internal::DecodeResolvedOperation(root);
}

std::string EncodeSceneTransaction(
    const core::SceneTransactionDTO &transaction) {
  CanonicalValue resolution = nullptr;
  if (transaction.resolution_evidence_digest.has_value()) {
    resolution = CanonicalValue(
        transaction.resolution_evidence_digest->value());
  }
  return ci::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"attribution",
       logical_scene_codec_internal::Attribution(transaction.attribution)},
      {"original_request_digest",
       CanonicalValue(transaction.original_request_digest.value())},
      {"prior_projection_revision",
       logical_scene_codec_internal::Projection(
           transaction.prior_projection_revision)},
      {"proposed_projection_revision",
       logical_scene_codec_internal::Projection(
           transaction.proposed_projection_revision)},
      {"request_id", CanonicalValue(transaction.request_id.value())},
      {"resolution_evidence_digest", std::move(resolution)},
      {"resolved_operation",
       logical_scene_codec_internal::ResolvedOperation(
           transaction.resolved_operation)},
      {"transaction_sequence",
       logical_scene_codec_internal::Unsigned(
           transaction.transaction_sequence, "transaction_sequence")},
  }));
}

core::SceneTransactionDTO
DecodeSceneTransaction(const std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const auto &root = parsed.root();
  if (ci::canonical_json(root) != bytes) {
    throw std::invalid_argument(
        "logical-scene codec: scene transaction is not canonical JSON");
  }
  logical_scene_codec_internal::ExactObject(root, 8U, "scene transaction");
  std::optional<core::ContentDigest> resolution;
  const auto &encoded_resolution =
      logical_scene_codec_internal::Member(root,
                                           "resolution_evidence_digest");
  if (!std::holds_alternative<std::nullptr_t>(
          encoded_resolution.storage())) {
    resolution = core::ContentDigest(logical_scene_codec_internal::String(
        encoded_resolution, "resolution_evidence_digest"));
  }
  core::SceneTransactionDTO transaction{
      core::RequestId(logical_scene_codec_internal::String(
          logical_scene_codec_internal::Member(root, "request_id"),
          "request_id")),
      logical_scene_codec_internal::DecodeAttribution(
          logical_scene_codec_internal::Member(root, "attribution")),
      logical_scene_codec_internal::DecodeUnsigned(
          logical_scene_codec_internal::Member(root, "transaction_sequence"),
          "transaction_sequence"),
      logical_scene_codec_internal::DecodeProjection(
          logical_scene_codec_internal::Member(
              root, "prior_projection_revision")),
      logical_scene_codec_internal::DecodeProjection(
          logical_scene_codec_internal::Member(
              root, "proposed_projection_revision")),
      core::ContentDigest(logical_scene_codec_internal::String(
          logical_scene_codec_internal::Member(root,
                                               "original_request_digest"),
          "original_request_digest")),
      logical_scene_codec_internal::DecodeResolvedOperation(
          logical_scene_codec_internal::Member(root, "resolved_operation")),
      std::move(resolution)};
  if (transaction.request_id.empty() || transaction.transaction_sequence == 0U ||
      !transaction.attribution.locally_valid() ||
      !transaction.prior_projection_revision.valid() ||
      !transaction.proposed_projection_revision.valid() ||
      transaction.original_request_digest.empty()) {
    throw std::invalid_argument(
        "logical-scene codec: decoded scene transaction is invalid");
  }
  return transaction;
}

std::string EncodeResolvedSceneCandidatePreviewRequest(
    const core::ResolvedSceneCandidatePreviewRequestDTO &request) {
  const auto encoded_request =
      logical_scene_codec_internal::EncodeRequest(request.request);
  const auto parsed_request =
      deployment::parse_strict_json_model_yaml(encoded_request);
  return ci::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"frame_resolution",
       request.frame_resolution.has_value()
           ? logical_scene_codec_internal::SceneFrameResolutionReceipt(
                 *request.frame_resolution)
           : CanonicalValue(nullptr)},
      {"request", parsed_request.root()},
  }));
}

core::ResolvedSceneCandidatePreviewRequestDTO
DecodeResolvedSceneCandidatePreviewRequest(const std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const auto &root = parsed.root();
  if (ci::canonical_json(root) != bytes) {
    throw std::invalid_argument(
        "logical-scene codec: scene candidate request is not canonical JSON");
  }
  logical_scene_codec_internal::ExactObject(root, 2U,
                                             "scene candidate request");
  std::optional<core::SceneFrameResolutionReceiptDTO> frame_resolution;
  const auto &encoded_frame =
      logical_scene_codec_internal::Member(root, "frame_resolution");
  if (!std::holds_alternative<std::nullptr_t>(encoded_frame.storage())) {
    frame_resolution =
        logical_scene_codec_internal::DecodeSceneFrameResolutionReceipt(
            encoded_frame);
  }
  return core::ResolvedSceneCandidatePreviewRequestDTO{
      logical_scene_codec_internal::DecodeRequest(ci::canonical_json(
          logical_scene_codec_internal::Member(root, "request"))),
      std::move(frame_resolution)};
}

std::string EncodeSceneCandidatePreview(
    const core::SceneCandidatePreviewDTO &preview) {
  if (!preview.valid()) {
    throw std::invalid_argument(
        "logical-scene codec: cannot encode an invalid scene candidate");
  }
  return ci::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"effective_collision_policy",
       logical_scene_codec_internal::Policy(
           preview.effective_collision_policy)},
      {"logical_scene",
       logical_scene_codec_internal::Snapshot(*preview.logical_scene)},
      {"prior_projection_revision",
       logical_scene_codec_internal::Projection(
           preview.prior_projection_revision)},
      {"proposed_projection_revision",
       logical_scene_codec_internal::Projection(
           preview.proposed_projection_revision)},
      {"request_digest", CanonicalValue(preview.request_digest.value())},
      {"resolution_evidence_digest",
       preview.resolution_evidence_digest.has_value()
           ? CanonicalValue(preview.resolution_evidence_digest->value())
           : CanonicalValue(nullptr)},
      {"resolved_operation",
       logical_scene_codec_internal::ResolvedOperation(
           preview.resolved_operation)},
      {"update_status",
       CanonicalValue(std::string(core::to_string(preview.update_status)))},
  }));
}

core::SceneCandidatePreviewDTO
DecodeSceneCandidatePreview(const std::string_view bytes) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  const auto &root = parsed.root();
  if (ci::canonical_json(root) != bytes) {
    throw std::invalid_argument(
        "logical-scene codec: scene candidate is not canonical JSON");
  }
  logical_scene_codec_internal::ExactObject(root, 8U, "scene candidate");
  std::optional<core::ContentDigest> resolution_evidence_digest;
  const auto &encoded_resolution = logical_scene_codec_internal::Member(
      root, "resolution_evidence_digest");
  if (!std::holds_alternative<std::nullptr_t>(
          encoded_resolution.storage())) {
    resolution_evidence_digest = core::ContentDigest(
        logical_scene_codec_internal::String(
            encoded_resolution, "resolution_evidence_digest"));
  }
  auto snapshot = std::make_shared<const core::LogicalSceneSnapshotDTO>(
      logical_scene_codec_internal::DecodeSnapshot(ci::canonical_json(
          logical_scene_codec_internal::Member(root, "logical_scene"))));
  core::SceneCandidatePreviewDTO result{
      core::ContentDigest(logical_scene_codec_internal::String(
          logical_scene_codec_internal::Member(root, "request_digest"),
          "request_digest")),
      logical_scene_codec_internal::DecodeProjection(
          logical_scene_codec_internal::Member(
              root, "prior_projection_revision")),
      logical_scene_codec_internal::DecodeProjection(
          logical_scene_codec_internal::Member(
              root, "proposed_projection_revision")),
      std::move(snapshot),
      logical_scene_codec_internal::DecodeResolvedOperation(
          logical_scene_codec_internal::Member(root, "resolved_operation")),
      logical_scene_codec_internal::DecodePolicy(
          logical_scene_codec_internal::Member(
              root, "effective_collision_policy")),
      logical_scene_codec_internal::DecodeSceneUpdateStatus(
          logical_scene_codec_internal::Member(root, "update_status")),
      std::move(resolution_evidence_digest)};
  if (!result.valid()) {
    throw std::invalid_argument(
        "logical-scene codec: decoded scene candidate is invalid");
  }
  return result;
}

} // namespace manyforge::scene

namespace manyforge::scene {

std::string EncodeCanonicalSceneTransactionRequest(
    const core::SceneTransactionRequestDTO &request) {
  return logical_scene_codec_internal::EncodeRequest(request);
}

core::SceneTransactionRequestDTO
DecodeCanonicalSceneTransactionRequest(const std::string_view bytes) {
  return logical_scene_codec_internal::DecodeRequest(bytes);
}

} // namespace manyforge::scene
