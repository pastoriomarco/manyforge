#include <manyforge_core/logical_scene_identity.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::core::logical_scene_identity {
namespace {

using manyforge::content_identity::CanonicalValue;
using manyforge::content_identity::CanonicalValueError;

constexpr std::uint64_t kMaxSafeIntegral = 9007199254740991ULL;
constexpr std::int64_t kMinSafeIntegral = -9007199254740991LL;
constexpr std::string_view kSceneObjectCollisionIdentityDomain =
    "scene-object-collision-content";

[[noreturn]] void Fail(std::string message) {
  throw LogicalSceneIdentityError(std::move(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(std::string(message));
  }
}

CanonicalValue String(std::string_view value, std::string_view field) {
  Require(!value.empty(), std::string(field) + " must not be empty");
  try {
    return CanonicalValue(std::string(value));
  } catch (const CanonicalValueError& error) {
    Fail(std::string(field) + ": " + error.what());
  }
}

CanonicalValue Unsigned(std::uint64_t value, std::string_view field,
                        bool nonzero = false) {
  Require(value <= kMaxSafeIntegral,
          std::string(field) + " exceeds the RFC-8785 safe-integral range");
  Require(!nonzero || value != 0U, std::string(field) + " must be nonzero");
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Signed(std::int64_t value, std::string_view field) {
  Require(value >= kMinSafeIntegral &&
              value <= static_cast<std::int64_t>(kMaxSafeIntegral),
          std::string(field) + " exceeds the RFC-8785 safe-integral range");
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(double value, std::string_view field,
                      bool positive = false, bool nonnegative = false) {
  Require(std::isfinite(value), std::string(field) + " must be finite");
  Require(!std::signbit(value) || value != 0.0,
          std::string(field) + " must not be negative zero");
  Require(!positive || value > 0.0, std::string(field) + " must be positive");
  Require(!nonnegative || value >= 0.0,
          std::string(field) + " must be nonnegative");
  return CanonicalValue(value);
}

CanonicalValue Bool(bool value) { return CanonicalValue(value); }

CanonicalValue Vector3(const Vector3DTO& value, std::string_view field,
                       bool positive = false) {
  return CanonicalValue(CanonicalValue::Object{
      {"x_m", Number(value.x_m, std::string(field) + ".x_m", positive)},
      {"y_m", Number(value.y_m, std::string(field) + ".y_m", positive)},
      {"z_m", Number(value.z_m, std::string(field) + ".z_m", positive)},
  });
}

CanonicalValue Quaternion(const QuaternionDTO& value, std::string_view field) {
  const double norm_squared = value.x * value.x + value.y * value.y +
                              value.z * value.z + value.w * value.w;
  Require(std::isfinite(norm_squared) && norm_squared > 0.0,
          std::string(field) + " must be a finite nonzero quaternion");
  return CanonicalValue(CanonicalValue::Object{
      {"x", Number(value.x, std::string(field) + ".x")},
      {"y", Number(value.y, std::string(field) + ".y")},
      {"z", Number(value.z, std::string(field) + ".z")},
      {"w", Number(value.w, std::string(field) + ".w")},
  });
}

CanonicalValue Pose(const Pose3DTO& value, std::string_view field) {
  return CanonicalValue(CanonicalValue::Object{
      {"position_m",
       Vector3(value.position_m, std::string(field) + ".position_m")},
      {"orientation",
       Quaternion(value.orientation, std::string(field) + ".orientation")},
  });
}

void RequireCanonicalPair(const CollisionPairId& pair, std::string_view field) {
  (void)String(pair.body_a, std::string(field) + ".body_a");
  (void)String(pair.body_b, std::string(field) + ".body_b");
  Require(pair.body_a <= pair.body_b,
          std::string(field) + " is not a canonical unordered pair");
}

CanonicalValue Pair(const CollisionPairId& pair, std::string_view field) {
  RequireCanonicalPair(pair, field);
  return CanonicalValue(CanonicalValue::Object{
      {"body_a", String(pair.body_a, std::string(field) + ".body_a")},
      {"body_b", String(pair.body_b, std::string(field) + ".body_b")},
  });
}

template <typename T, typename Less>
void RequireStrictlyIncreasing(const std::vector<T>& values, Less less,
                               std::string_view field) {
  for (std::size_t index = 1U; index < values.size(); ++index) {
    Require(less(values[index - 1U], values[index]),
            std::string(field) + " must be sorted and unique");
  }
}

CanonicalValue PairSet(const std::vector<CollisionPairId>& values,
                       std::string_view field) {
  RequireStrictlyIncreasing(
      values, [](const auto& left, const auto& right) { return left < right; },
      field);
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (std::size_t index = 0U; index < values.size(); ++index) {
    result.push_back(Pair(
        values[index], std::string(field) + "[" + std::to_string(index) + "]"));
  }
  return CanonicalValue(std::move(result));
}

std::string_view ContactMode(CollisionContactPolicyMode mode) {
  switch (mode) {
    case CollisionContactPolicyMode::kContactRelease:
      return "contact_release";
    case CollisionContactPolicyMode::kBoundedPenetration:
      return "bounded_penetration";
    case CollisionContactPolicyMode::kContactProcess:
      return "contact_process";
  }
  Fail("support contact has an unknown mode");
}

std::string_view ReleaseEvidence(ContactReleaseEvidenceMode mode) {
  switch (mode) {
    case ContactReleaseEvidenceMode::kSignedPenetration:
      return "signed_penetration";
    case ContactReleaseEvidenceMode::kBinaryContact:
      return "binary_contact";
  }
  Fail("support contact has an unknown release-evidence mode");
}

void RequireNormalizedSupportContact(const SupportContactPolicyEntry& entry,
                                     std::string_view field) {
  RequireCanonicalPair(entry.pair, std::string(field) + ".pair");
  (void)Number(entry.max_start_penetration_m,
               std::string(field) + ".max_start_penetration_m", false, true);
  (void)Number(entry.release_tolerance_m,
               std::string(field) + ".release_tolerance_m", false, true);
  (void)Number(entry.min_release_progress_m,
               std::string(field) + ".min_release_progress_m", false, true);
  (void)Number(entry.max_contact_penetration_m,
               std::string(field) + ".max_contact_penetration_m", false, true);

  if (entry.mode == CollisionContactPolicyMode::kContactRelease) {
    Require(entry.max_contact_penetration_m == 0.0,
            std::string(field) + ".max_contact_penetration_m must be neutral");
    if (entry.release_evidence_mode ==
        ContactReleaseEvidenceMode::kBinaryContact) {
      Require(
          !entry.enforce_max_start_penetration &&
              entry.max_start_penetration_m == 0.0 &&
              entry.release_tolerance_m == 0.0 &&
              entry.min_release_progress_m == 0.0 &&
              !entry.require_strictly_decreasing_penetration,
          std::string(field) + " binary-contact metric fields must be neutral");
    }
    return;
  }

  Require(entry.release_evidence_mode ==
                  ContactReleaseEvidenceMode::kSignedPenetration &&
              !entry.enforce_max_start_penetration &&
              entry.max_start_penetration_m == 0.0 &&
              entry.release_tolerance_m == 0.0 &&
              entry.min_release_progress_m == 0.0 &&
              !entry.require_separation_by_end &&
              !entry.require_strictly_decreasing_penetration &&
              !entry.allow_recontact,
          std::string(field) + " mode-scoped release fields must be neutral");
  if (entry.mode == CollisionContactPolicyMode::kContactProcess) {
    Require(entry.max_contact_penetration_m == 0.0,
            std::string(field) + ".max_contact_penetration_m must be neutral");
  }
}

CanonicalValue SupportContact(const SupportContactPolicyEntry& entry,
                              std::string_view field) {
  RequireNormalizedSupportContact(entry, field);
  return CanonicalValue(CanonicalValue::Object{
      {"pair", Pair(entry.pair, std::string(field) + ".pair")},
      {"mode", CanonicalValue(std::string(ContactMode(entry.mode)))},
      {"release_evidence_mode", CanonicalValue(std::string(ReleaseEvidence(
                                    entry.release_evidence_mode)))},
      {"enforce_max_start_penetration",
       Bool(entry.enforce_max_start_penetration)},
      {"max_start_penetration_m",
       Number(entry.max_start_penetration_m,
              std::string(field) + ".max_start_penetration_m", false, true)},
      {"release_tolerance_m",
       Number(entry.release_tolerance_m,
              std::string(field) + ".release_tolerance_m", false, true)},
      {"min_release_progress_m",
       Number(entry.min_release_progress_m,
              std::string(field) + ".min_release_progress_m", false, true)},
      {"max_contact_penetration_m",
       Number(entry.max_contact_penetration_m,
              std::string(field) + ".max_contact_penetration_m", false, true)},
      {"require_separation_by_end", Bool(entry.require_separation_by_end)},
      {"require_strictly_decreasing_penetration",
       Bool(entry.require_strictly_decreasing_penetration)},
      {"allow_recontact", Bool(entry.allow_recontact)},
  });
}

CanonicalValue SupportContacts(
    const std::vector<SupportContactPolicyEntry>& entries) {
  RequireStrictlyIncreasing(
      entries,
      [](const auto& left, const auto& right) {
        return left.pair < right.pair;
      },
      "support_contacts");
  CanonicalValue::Array result;
  result.reserve(entries.size());
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    result.push_back(SupportContact(
        entries[index], "support_contacts[" + std::to_string(index) + "]"));
  }
  return CanonicalValue(std::move(result));
}

void RequireNormalizedSupportDeclaration(const SupportContactDTO& entry,
                                         std::string_view field) {
  (void)String(entry.support_object_id,
               std::string(field) + ".support_object_id");
  (void)Number(entry.max_start_penetration_m,
               std::string(field) + ".max_start_penetration_m", false, true);
  (void)Number(entry.release_tolerance_m,
               std::string(field) + ".release_tolerance_m", false, true);
  (void)Number(entry.min_release_progress_m,
               std::string(field) + ".min_release_progress_m", false, true);
  (void)Number(entry.max_contact_penetration_m,
               std::string(field) + ".max_contact_penetration_m", false, true);
  Require(logical_scene_detail::NormalizedSupportContact(entry),
          std::string(field) + " is not a normalized support declaration");
}

CanonicalValue SupportDeclaration(const SupportContactDTO& entry,
                                  std::string_view field) {
  RequireNormalizedSupportDeclaration(entry, field);
  return CanonicalValue(CanonicalValue::Object{
      {"support_object_id", String(entry.support_object_id,
                                   std::string(field) + ".support_object_id")},
      {"mode", CanonicalValue(std::string(ContactMode(entry.mode)))},
      {"release_evidence_mode", CanonicalValue(std::string(ReleaseEvidence(
                                    entry.release_evidence_mode)))},
      {"enforce_max_start_penetration",
       Bool(entry.enforce_max_start_penetration)},
      {"max_start_penetration_m",
       Number(entry.max_start_penetration_m,
              std::string(field) + ".max_start_penetration_m", false, true)},
      {"release_tolerance_m",
       Number(entry.release_tolerance_m,
              std::string(field) + ".release_tolerance_m", false, true)},
      {"min_release_progress_m",
       Number(entry.min_release_progress_m,
              std::string(field) + ".min_release_progress_m", false, true)},
      {"max_contact_penetration_m",
       Number(entry.max_contact_penetration_m,
              std::string(field) + ".max_contact_penetration_m", false, true)},
      {"require_separation_by_end", Bool(entry.require_separation_by_end)},
      {"require_strictly_decreasing_penetration",
       Bool(entry.require_strictly_decreasing_penetration)},
      {"allow_recontact", Bool(entry.allow_recontact)},
  });
}

CanonicalValue SupportDeclarations(
    const std::vector<SupportContactDTO>& entries, std::string_view field) {
  RequireStrictlyIncreasing(
      entries,
      [](const auto& left, const auto& right) {
        return left.support_object_id < right.support_object_id;
      },
      field);
  CanonicalValue::Array result;
  result.reserve(entries.size());
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    result.push_back(SupportDeclaration(
        entries[index],
        std::string(field) + "[" + std::to_string(index) + "]"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue ContactGrasp(const ContactGraspPolicyEntry& entry,
                            std::string_view field) {
  (void)String(entry.policy_id, std::string(field) + ".policy_id");
  (void)String(entry.selected_object_id,
               std::string(field) + ".selected_object_id");
  RequireStrictlyIncreasing(entry.allowed_robot_links, std::less<std::string>{},
                            std::string(field) + ".allowed_robot_links");
  CanonicalValue::Array links;
  links.reserve(entry.allowed_robot_links.size());
  for (std::size_t index = 0U; index < entry.allowed_robot_links.size();
       ++index) {
    links.push_back(String(entry.allowed_robot_links[index],
                           std::string(field) + ".allowed_robot_links[" +
                               std::to_string(index) + "]"));
  }
  Require(entry.terminal_window_start_fraction >= 0.0 &&
              entry.terminal_window_start_fraction <= 1.0,
          std::string(field) +
              ".terminal_window_start_fraction must be within [0, 1]");
  return CanonicalValue(CanonicalValue::Object{
      {"policy_id", String(entry.policy_id, std::string(field) + ".policy_id")},
      {"selected_object_id",
       String(entry.selected_object_id,
              std::string(field) + ".selected_object_id")},
      {"allowed_robot_links", CanonicalValue(std::move(links))},
      {"max_penetration_m",
       Number(entry.max_penetration_m,
              std::string(field) + ".max_penetration_m", false, true)},
      {"terminal_window_start_fraction",
       Number(entry.terminal_window_start_fraction,
              std::string(field) + ".terminal_window_start_fraction")},
  });
}

CanonicalValue ContactGrasps(
    const std::vector<ContactGraspPolicyEntry>& entries) {
  RequireStrictlyIncreasing(
      entries,
      [](const auto& left, const auto& right) {
        return left.policy_id < right.policy_id;
      },
      "contact_grasps");
  CanonicalValue::Array result;
  result.reserve(entries.size());
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    result.push_back(ContactGrasp(
        entries[index], "contact_grasps[" + std::to_string(index) + "]"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue PolicySemantics(const CollisionPolicySnapshot& policy) {
  return CanonicalValue(CanonicalValue::Object{
      {"policy_id", String(policy.policy_id, "policy_id")},
      {"allowed_static_static_pairs",
       PairSet(policy.allowed_static_static_pairs,
               "allowed_static_static_pairs")},
      {"allowed_touch_link_pairs",
       PairSet(policy.allowed_touch_link_pairs, "allowed_touch_link_pairs")},
      {"support_contacts", SupportContacts(policy.support_contacts)},
      {"contact_grasps", ContactGrasps(policy.contact_grasps)},
      {"require_full_diagnostics", Bool(policy.require_full_diagnostics)},
  });
}

ContentDigest PolicyDigest(const CollisionPolicySnapshot& policy) {
  return ContentDigest(manyforge::content_identity::framed_sha256(
      kCollisionPolicyContentIdentityDomain, kCollisionPolicyApiVersion,
      PolicySemantics(policy)));
}

CanonicalValue CompletePolicy(const CollisionPolicySnapshot& policy) {
  const CanonicalValue semantics = PolicySemantics(policy);
  const ContentDigest computed(manyforge::content_identity::framed_sha256(
      kCollisionPolicyContentIdentityDomain, kCollisionPolicyApiVersion,
      semantics));
  Require(policy.revision.revision_id != 0U,
          "collision policy revision_id must be nonzero");
  (void)Unsigned(policy.revision.revision_id, "collision policy revision_id",
                 true);
  Require(manyforge::content_identity::is_sha256_digest(
              policy.revision.content_hash),
          "collision policy content_hash has invalid spelling");
  Require(manyforge::content_identity::digest_equal(
              policy.revision.content_hash, computed.value()),
          "collision policy content_hash does not match its semantic policy");

  const auto& semantic_members = semantics.as_object();
  CanonicalValue::Object object(semantic_members.begin(),
                                semantic_members.end());
  object.emplace_back(
      "revision",
      CanonicalValue(CanonicalValue::Object{
          {"revision_id", Unsigned(policy.revision.revision_id,
                                   "collision policy revision_id", true)},
          {"content_hash", String(policy.revision.content_hash,
                                  "collision policy content_hash")},
      }));
  return CanonicalValue(std::move(object));
}

CanonicalValue Shape(const CollisionShapeDTO& shape, std::string_view field) {
  return std::visit(
      [&](const auto& value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, BoxShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"type", CanonicalValue("box")},
              {"box_dimensions_m",
               Vector3(value.dimensions_m,
                       std::string(field) + ".box_dimensions_m", true)},
          });
        } else if constexpr (std::is_same_v<T, SphereShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"type", CanonicalValue("sphere")},
              {"sphere_radius_m",
               Number(value.radius_m, std::string(field) + ".sphere_radius_m",
                      true)},
          });
        } else if constexpr (std::is_same_v<T, CylinderShapeDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"type", CanonicalValue("cylinder")},
              {"cylinder_radius_m",
               Number(value.radius_m, std::string(field) + ".cylinder_radius_m",
                      true)},
              {"cylinder_height_m",
               Number(value.height_m, std::string(field) + ".cylinder_height_m",
                      true)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"type", CanonicalValue("mesh")},
              {"mesh_resource_uri",
               String(value.resource_uri,
                      std::string(field) + ".mesh_resource_uri")},
              {"mesh_scale",
               Vector3(value.scale, std::string(field) + ".mesh_scale", true)},
          });
        }
      },
      shape);
}

CanonicalValue ShapeSet(const std::vector<CollisionShapeDTO>& shapes,
                        std::string_view field) {
  Require(!shapes.empty(), std::string(field) + " must not be empty");
  std::vector<std::pair<std::string, CanonicalValue>> rows;
  rows.reserve(shapes.size());
  for (std::size_t index = 0U; index < shapes.size(); ++index) {
    CanonicalValue row = Shape(
        shapes[index], std::string(field) + "[" + std::to_string(index) + "]");
    rows.emplace_back(manyforge::content_identity::canonical_json(row),
                      std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });
  CanonicalValue::Array result;
  result.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    Require(index == 0U || rows[index - 1U].first != rows[index].first,
            std::string(field) + " contains a duplicate shape");
    result.push_back(std::move(rows[index].second));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue Placement(const SceneObjectPlacementDTO& placement,
                         std::string_view field, const CellId& snapshot_cell) {
  return std::visit(
      [&](const auto& value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, CellFixedSceneObjectPlacementDTO>) {
          Require(
              value.cell_id == snapshot_cell,
              std::string(field) + ".cell_id must equal the snapshot cell_id");
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("cell_fixed")},
              {"cell_id",
               String(value.cell_id.value(), std::string(field) + ".cell_id")},
              {"pose_in_cell",
               Pose(value.pose_in_cell, std::string(field) + ".pose_in_cell")},
          });
        } else {
          Require(manyforge::content_identity::is_sha256_digest(
                      value.carried_object_attachment_revision.value()),
                  std::string(field) +
                      ".carried_object_attachment_revision has invalid "
                      "spelling");
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("carried")},
              {"carried_object_attachment_id",
               String(value.carried_object_attachment_id.value(),
                      std::string(field) + ".carried_object_attachment_id")},
              {"carried_object_attachment_revision",
               String(
                   value.carried_object_attachment_revision.value(),
                   std::string(field) + ".carried_object_attachment_revision")},
          });
        }
      },
      placement);
}

CanonicalValue ProviderEvidence(const SceneObjectProviderEvidenceDTO& value,
                                std::string_view field) {
  Require(
      manyforge::content_identity::is_sha256_digest(
          value.provider_manifest_revision.value()),
      std::string(field) + ".provider_manifest_revision has invalid spelling");
  CanonicalValue::Object object{
      {"provider_instance_id",
       String(value.provider_instance_id.value(),
              std::string(field) + ".provider_instance_id")},
      {"provider_manifest_id",
       String(value.provider_manifest_id.value(),
              std::string(field) + ".provider_manifest_id")},
      {"provider_manifest_revision",
       String(value.provider_manifest_revision.value(),
              std::string(field) + ".provider_manifest_revision")},
      {"provider_source_epoch",
       String(value.provider_source_epoch,
              std::string(field) + ".provider_source_epoch")},
      {"provider_source_sequence",
       Unsigned(value.provider_source_sequence,
                std::string(field) + ".provider_source_sequence")},
      {"received_at_ns",
       Signed(value.received_at_ns, std::string(field) + ".received_at_ns")},
  };
  if (value.source_timestamp_ns.has_value()) {
    object.emplace_back("source_timestamp_ns",
                        Signed(*value.source_timestamp_ns,
                               std::string(field) + ".source_timestamp_ns"));
  }
  if (value.source_freshness_budget_ns.has_value()) {
    Require(
        *value.source_freshness_budget_ns > 0,
        std::string(field) + ".source_freshness_budget_ns must be positive");
    object.emplace_back(
        "source_freshness_budget_ns",
        Signed(*value.source_freshness_budget_ns,
               std::string(field) + ".source_freshness_budget_ns"));
  }
  return CanonicalValue(std::move(object));
}

CanonicalValue SceneObject(const SceneObjectSnapshotDTO& object,
                           std::string_view field,
                           const CellId& snapshot_cell) {
  CanonicalValue::Object result{
      {"scene_resource_id", String(object.scene_resource_id.value(),
                                   std::string(field) + ".scene_resource_id")},
      {"collision_shapes", ShapeSet(object.collision_shapes,
                                    std::string(field) + ".collision_shapes")},
      {"placement",
       Placement(object.placement, std::string(field) + ".placement",
                 snapshot_cell)},
      {"support_contacts",
       SupportDeclarations(object.support_contacts,
                           std::string(field) + ".support_contacts")},
  };
  if (object.provider_evidence.has_value()) {
    result.emplace_back(
        "provider_evidence",
        ProviderEvidence(*object.provider_evidence,
                         std::string(field) + ".provider_evidence"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue FrameRef(const SceneFrameRefDTO& frame, std::string_view field) {
  return std::visit(
      [&](const auto& value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, CellSceneFrameRefDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("cell_frame")},
              {"cell_id",
               String(value.cell_id.value(), std::string(field) + ".cell_id")},
          });
        } else if constexpr (std::is_same_v<T, ComponentSceneFrameRefDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("component_frame")},
              {"controllable_component_id",
               String(value.controllable_component_id.value(),
                      std::string(field) + ".controllable_component_id")},
              {"model_instance_id",
               String(value.model_instance_id.value(),
                      std::string(field) + ".model_instance_id")},
              {"frame_role_id", String(value.frame_role_id.value(),
                                       std::string(field) + ".frame_role_id")},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("passive_model_frame")},
              {"model_instance_id",
               String(value.model_instance_id.value(),
                      std::string(field) + ".model_instance_id")},
              {"frame_role_id", String(value.frame_role_id.value(),
                                       std::string(field) + ".frame_role_id")},
          });
        }
      },
      frame);
}

CanonicalValue AttachmentPayload(const CarriedObjectAttachmentDTO& attachment,
                                 std::string_view field) {
  const auto* component_frame =
      std::get_if<ComponentSceneFrameRefDTO>(&attachment.carrier_frame_ref);
  Require(component_frame != nullptr,
          std::string(field) + ".carrier_frame_ref must be a component frame");
  Require(component_frame->controllable_component_id ==
              attachment.controllable_component_id,
          std::string(field) +
              ".carrier_frame_ref component does not own the attachment");
  RequireStrictlyIncreasing(
      attachment.touch_body_roles,
      [](const auto& left, const auto& right) {
        return left.value() < right.value();
      },
      std::string(field) + ".touch_body_roles");
  CanonicalValue::Array touch_roles;
  touch_roles.reserve(attachment.touch_body_roles.size());
  for (std::size_t index = 0U; index < attachment.touch_body_roles.size();
       ++index) {
    touch_roles.push_back(String(attachment.touch_body_roles[index].value(),
                                 std::string(field) + ".touch_body_roles[" +
                                     std::to_string(index) + "]"));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"carried_object_attachment_id",
       String(attachment.carried_object_attachment_id.value(),
              std::string(field) + ".carried_object_attachment_id")},
      {"scene_resource_id", String(attachment.scene_resource_id.value(),
                                   std::string(field) + ".scene_resource_id")},
      {"controllable_component_id",
       String(attachment.controllable_component_id.value(),
              std::string(field) + ".controllable_component_id")},
      {"command_target_id", String(attachment.command_target_id.value(),
                                   std::string(field) + ".command_target_id")},
      {"carrier_frame_ref",
       FrameRef(attachment.carrier_frame_ref,
                std::string(field) + ".carrier_frame_ref")},
      {"pose_in_carrier_frame",
       Pose(attachment.pose_in_carrier_frame,
            std::string(field) + ".pose_in_carrier_frame")},
      {"touch_body_roles", CanonicalValue(std::move(touch_roles))},
  });
}

CarriedObjectAttachmentRevision AttachmentRevision(
    const CarriedObjectAttachmentDTO& attachment, std::string_view field) {
  return CarriedObjectAttachmentRevision(
      manyforge::content_identity::framed_sha256(
          kCarriedObjectAttachmentRevisionIdentityDomain,
          kLogicalSceneApiVersion, AttachmentPayload(attachment, field)));
}

CanonicalValue CompleteAttachment(const CarriedObjectAttachmentDTO& attachment,
                                  std::string_view field) {
  Require(manyforge::content_identity::is_sha256_digest(
              attachment.carried_object_attachment_revision.value()),
          std::string(field) +
              ".carried_object_attachment_revision has invalid spelling");
  const CanonicalValue payload = AttachmentPayload(attachment, field);
  const CarriedObjectAttachmentRevision computed(
      manyforge::content_identity::framed_sha256(
          kCarriedObjectAttachmentRevisionIdentityDomain,
          kLogicalSceneApiVersion, payload));
  Require(manyforge::content_identity::digest_equal(
              attachment.carried_object_attachment_revision.value(),
              computed.value()),
          std::string(field) +
              ".carried_object_attachment_revision does not match its "
              "semantic attachment");
  const auto& payload_members = payload.as_object();
  CanonicalValue::Object result(payload_members.begin(), payload_members.end());
  result.emplace_back(
      "carried_object_attachment_revision",
      String(attachment.carried_object_attachment_revision.value(),
             std::string(field) + ".carried_object_attachment_revision"));
  return CanonicalValue(std::move(result));
}

CanonicalValue SceneContent(const LogicalSceneSnapshotDTO& snapshot) {
  Require(logical_scene_detail::LocallyValidSceneContent(snapshot),
          "logical scene content is invalid");
  (void)String(snapshot.cell_id.value(), "cell_id");
  RequireStrictlyIncreasing(
      snapshot.objects,
      [](const auto& left, const auto& right) {
        return left.scene_resource_id < right.scene_resource_id;
      },
      "objects");
  RequireStrictlyIncreasing(
      snapshot.carried_object_attachments,
      [](const auto& left, const auto& right) {
        return left.carried_object_attachment_id <
               right.carried_object_attachment_id;
      },
      "carried_object_attachments");

  CanonicalValue::Array objects;
  objects.reserve(snapshot.objects.size());
  for (std::size_t index = 0U; index < snapshot.objects.size(); ++index) {
    objects.push_back(SceneObject(snapshot.objects[index],
                                  "objects[" + std::to_string(index) + "]",
                                  snapshot.cell_id));
  }
  CanonicalValue::Array attachments;
  attachments.reserve(snapshot.carried_object_attachments.size());
  for (std::size_t index = 0U;
       index < snapshot.carried_object_attachments.size(); ++index) {
    attachments.push_back(CompleteAttachment(
        snapshot.carried_object_attachments[index],
        "carried_object_attachments[" + std::to_string(index) + "]"));
  }

  std::size_t carried_count = 0U;
  for (const auto& object : snapshot.objects) {
    const auto* carried =
        std::get_if<CarriedSceneObjectPlacementDTO>(&object.placement);
    if (carried == nullptr) {
      continue;
    }
    ++carried_count;
    const auto found =
        std::lower_bound(snapshot.carried_object_attachments.begin(),
                         snapshot.carried_object_attachments.end(),
                         carried->carried_object_attachment_id,
                         [](const auto& attachment, const auto& id) {
                           return attachment.carried_object_attachment_id < id;
                         });
    Require(found != snapshot.carried_object_attachments.end() &&
                found->carried_object_attachment_id ==
                    carried->carried_object_attachment_id &&
                found->carried_object_attachment_revision ==
                    carried->carried_object_attachment_revision &&
                found->scene_resource_id == object.scene_resource_id,
            "carried object and attachment rows are inconsistent");
  }
  Require(carried_count == snapshot.carried_object_attachments.size(),
          "every carried attachment must have exactly one carried object");

  return CanonicalValue(CanonicalValue::Object{
      {"carried_object_attachments", CanonicalValue(std::move(attachments))},
      {"cell_id", String(snapshot.cell_id.value(), "cell_id")},
      {"configured_collision_policy",
       CompletePolicy(snapshot.configured_collision_policy)},
      {"objects", CanonicalValue(std::move(objects))},
  });
}

template <typename Result, typename Function>
Result TranslateCodecErrors(Function function) {
  try {
    return function();
  } catch (const LogicalSceneIdentityError&) {
    throw;
  } catch (const CanonicalValueError& error) {
    throw LogicalSceneIdentityError(error.what());
  }
}

}  // namespace

SceneId MintSceneId(const LogicalSceneBootstrapIdentityInputDTO& input) {
  return TranslateCodecErrors<SceneId>([&] {
    Require(input.valid(), "logical-scene bootstrap identity input is invalid");
    const CanonicalValue payload(CanonicalValue::Object{
        {"cell_id", String(input.cell_id.value(), "cell_id")},
        {"bootstrap_id", String(input.bootstrap_id.value(), "bootstrap_id")},
        {"bootstrap_generation", Unsigned(input.bootstrap_generation.value(),
                                          "bootstrap_generation", true)},
    });
    return SceneId(manyforge::content_identity::framed_sha256(
        kLogicalSceneIdIdentityDomain, kLogicalSceneApiVersion, payload));
  });
}

ContentDigest ComputeCollisionPolicyContentDigest(
    const CollisionPolicySnapshot& normalized_policy) {
  return TranslateCodecErrors<ContentDigest>(
      [&] { return PolicyDigest(normalized_policy); });
}

CarriedObjectAttachmentRevision ComputeCarriedObjectAttachmentRevision(
    const CarriedObjectAttachmentDTO& normalized_attachment) {
  return TranslateCodecErrors<CarriedObjectAttachmentRevision>([&] {
    return AttachmentRevision(normalized_attachment,
                              "carried_object_attachment");
  });
}

ContentDigest ComputeSceneObjectCollisionIdentity(
    const SceneObjectSnapshotDTO& normalized_object,
    const CellId& snapshot_cell) {
  return TranslateCodecErrors<ContentDigest>([&] {
    // Validate the complete supplied row before projecting its deliberately
    // stable non-pose identity. This keeps unsafe sample counters and poses
    // from being silently normalized into an otherwise valid token.
    (void)SceneObject(normalized_object, "scene_object", snapshot_cell);

    auto identity_object = normalized_object;
    if (auto* fixed = std::get_if<CellFixedSceneObjectPlacementDTO>(
            &identity_object.placement)) {
      fixed->pose_in_cell = {
          {0.0, 0.0, 0.0},
          {0.0, 0.0, 0.0, 1.0},
      };
    }
    if (identity_object.provider_evidence.has_value()) {
      auto& evidence = *identity_object.provider_evidence;
      evidence.provider_source_sequence = 0U;
      evidence.received_at_ns = 0;
      evidence.source_timestamp_ns.reset();
    }
    return ContentDigest(manyforge::content_identity::framed_sha256(
        kSceneObjectCollisionIdentityDomain, kLogicalSceneApiVersion,
        SceneObject(identity_object, "scene_object", snapshot_cell)));
  });
}

ContentDigest ComputeLogicalSceneContentHash(
    const LogicalSceneSnapshotDTO& normalized_snapshot) {
  return TranslateCodecErrors<ContentDigest>([&] {
    return ContentDigest(manyforge::content_identity::framed_sha256(
        kLogicalSceneContentIdentityDomain, kLogicalSceneApiVersion,
        SceneContent(normalized_snapshot)));
  });
}

}  // namespace manyforge::core::logical_scene_identity
