#include "manyforge_deployment/verified_retained_bundle.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "control_domain_control_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/model_asset_codec.hpp"
#include "manyforge_deployment/model_source.hpp"
#include "catalog_artifact_store_internal.hpp"
#include "composed_model_symbol_role_map_internal.hpp"
#include "planning_resource_map_internal.hpp"
#include "posix_durability_internal.hpp"
#include "reference_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace detail {

class VerifiedRetainedPlanningConstructionAuthority final {
 public:
  [[nodiscard]] static VerifiedRetainedPlanningResource resource(
      std::string logical_uri, std::string media_type,
      std::uint64_t byte_size, core::ContentDigest content_digest,
      core::ContentAddressedStoreKey content_addressed_store_key,
      std::string bytes) {
    return VerifiedRetainedPlanningResource(
        std::move(logical_uri), std::move(media_type), byte_size,
        std::move(content_digest),
        std::move(content_addressed_store_key), std::move(bytes));
  }
};

}  // namespace detail

namespace {

namespace posix = detail::posix_durability;

[[nodiscard]] std::exception_ptr make_retention_error(
    std::string_view message) {
  return std::make_exception_ptr(RetainedBundleError(std::string(message)));
}

const posix::ErrorPolicy kPosixErrors{make_retention_error};
constexpr posix::TrustedRootMessages kTrustedRootMessages{
    "durable retention trusted root must not be empty",
    "cannot resolve durable retention trusted root",
    "durable retention trusted root must be an existing plain directory",
};
constexpr posix::ExistingDirectoryMessages kReadOnlyDirectoryMessages{
    "cannot inspect read-only durable retention directory",
    "read-only durable retention path must be an existing plain directory",
};
constexpr posix::ReadMessages kReadMessages{
    "cannot open durable retention entry",
    "cannot stat durable retention entry",
    "durable retention entry violates its immutable byte bound",
    "durable retention entry changed during exact read",
    "durable retention entry grew during exact read",
};

struct ReadOnlyRetentionPaths final {
  std::filesystem::path bundle_data;
  std::filesystem::path raw_data;
};

[[nodiscard]] ReadOnlyRetentionPaths require_read_only_retention_paths(
    const std::filesystem::path& trusted_root) {
  const std::filesystem::path bundle_namespace =
      trusted_root / "retained-bundles";
  const std::filesystem::path bundle_data =
      bundle_namespace / "sha256";
  const std::filesystem::path raw_namespace =
      trusted_root / "raw-artifacts";
  const std::filesystem::path raw_data = raw_namespace / "sha256";
  for (const auto& directory :
       {bundle_namespace, bundle_data, raw_namespace, raw_data}) {
    posix::require_existing_plain_directory(
        directory, kPosixErrors, kReadOnlyDirectoryMessages);
  }
  return {bundle_data, raw_data};
}

[[nodiscard]] std::filesystem::path require_read_only_catalog_data(
    const std::filesystem::path& trusted_root) {
  const std::filesystem::path catalog_namespace =
      trusted_root /
      detail::kCatalogArtifactNamespaceDirectory;
  const std::filesystem::path catalog_data =
      catalog_namespace /
      detail::kCatalogArtifactDataDirectory;
  for (const auto& directory :
       {catalog_namespace, catalog_data}) {
    posix::require_existing_plain_directory(
        directory, kPosixErrors, kReadOnlyDirectoryMessages);
  }
  return catalog_data;
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
read_verified_bundle_entry(
    const std::filesystem::path& bundle_data,
    const core::RetainedBundleId& retained_bundle_id) {
  const auto hex =
      detail::retained_digest_hex(retained_bundle_id.value());
  if (!hex.has_value()) {
    return nullptr;
  }
  const auto bytes = posix::read_exact_regular_file(
      bundle_data / (std::string(*hex) + ".json"),
      kMaxDurableRetainedBundleBytes, true, kPosixErrors,
      kReadMessages);
  if (!bytes.has_value()) {
    return nullptr;
  }
  const auto bundle =
      detail::RetainedBundleReloadAuthority::reload(*bytes);
  if (bundle->retained_bundle_id() != retained_bundle_id) {
    detail::fail_retained_bundle(
        "read-only retained-bundle key/content mismatch");
  }
  return bundle;
}

[[nodiscard]] std::string read_verified_raw_reference(
    const std::filesystem::path& raw_data,
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& digest,
    std::optional<std::uint64_t> expected_size,
    std::string_view reference_label) {
  const auto hex = detail::retained_raw_key_hex(key, digest);
  if (!hex.has_value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " has an inconsistent digest-derived raw-CAS key");
  }
  auto bytes = posix::read_exact_regular_file(
      raw_data / (std::string(*hex) + ".blob"),
      kMaxDurableRetainedBlobBytes, true, kPosixErrors,
      kReadMessages);
  if (!bytes.has_value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " is absent from the durable raw-artifact CAS");
  }
  if ((expected_size.has_value() &&
       static_cast<std::uint64_t>(bytes->size()) != *expected_size) ||
      sha256_digest(*bytes) != digest.value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " failed exact durable raw-artifact verification");
  }
  return std::move(*bytes);
}

[[nodiscard]] std::string read_verified_catalog_reference(
    const std::filesystem::path& catalog_data,
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& digest,
    std::optional<std::uint64_t> expected_size,
    std::string_view reference_label) {
  const auto storage_hex =
      detail::catalog_artifact_storage_hex(key.value());
  if (!storage_hex.has_value() ||
      !is_sha256_digest(digest.value())) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " has invalid exact catalog-artifact coordinates");
  }
  constexpr std::uint64_t kMaximumEnvelopeBytes =
      kMaxDurableRetainedBlobBytes +
      detail::kMaximumCatalogArtifactEnvelopeOverhead;
  auto envelope = posix::read_exact_regular_file(
      catalog_data /
          (std::string(*storage_hex) +
           std::string(detail::kCatalogArtifactFileSuffix)),
      kMaximumEnvelopeBytes, true, kPosixErrors, kReadMessages);
  if (!envelope.has_value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " is absent from the durable catalog-artifact store");
  }
  detail::DecodedCatalogArtifactEnvelope decoded =
      detail::decode_catalog_artifact_envelope(*envelope);
  if (decoded.content_addressed_store_key != key.value() ||
      decoded.content_digest != digest.value() ||
      (expected_size.has_value() &&
       decoded.payload.size() != *expected_size)) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " failed exact durable catalog-artifact verification");
  }
  return std::string(decoded.payload);
}

[[nodiscard]] const CanonicalValue& require_map_member(
    const CanonicalValue& value, std::string_view name) {
  const CanonicalValue* member = value.find_object_member(name);
  if (member == nullptr) {
    detail::fail_retained_bundle(
        "verified symbol/role map omitted " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_map_text(
    const CanonicalValue& value, std::string_view name) {
  const auto* text = std::get_if<std::string>(
      &require_map_member(value, name).storage());
  if (text == nullptr) {
    detail::fail_retained_bundle(
        "verified symbol/role map member is not text: " +
        std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array& require_map_array(
    const CanonicalValue& value, std::string_view name) {
  const auto* rows = std::get_if<CanonicalValue::Array>(
      &require_map_member(value, name).storage());
  if (rows == nullptr) {
    detail::fail_retained_bundle(
        "verified symbol/role map member is not an array: " +
        std::string(name));
  }
  return *rows;
}

[[nodiscard]] std::optional<std::string> require_map_optional_text(
    const CanonicalValue& value, std::string_view name) {
  const CanonicalValue& member = require_map_member(value, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto* text = std::get_if<std::string>(&member.storage());
  if (text == nullptr) {
    detail::fail_retained_bundle(
        "verified symbol/role map optional member is neither null nor text: " +
        std::string(name));
  }
  return *text;
}

[[nodiscard]] double require_map_number(
    const CanonicalValue& value, std::string_view name) {
  const auto* number = std::get_if<double>(
      &require_map_member(value, name).storage());
  if (number == nullptr || !std::isfinite(*number)) {
    detail::fail_retained_bundle(
        "verified symbol/role map member is not a finite number: " +
        std::string(name));
  }
  return *number;
}

[[nodiscard]] std::size_t require_map_index(
    const CanonicalValue& value, std::string_view name) {
  const double number = require_map_number(value, name);
  constexpr double kMaximumExactJsonInteger = 9007199254740991.0;
  if (number < 0.0 || number > kMaximumExactJsonInteger ||
      std::floor(number) != number ||
      number > static_cast<double>(
                   std::numeric_limits<std::size_t>::max())) {
    detail::fail_retained_bundle(
        "verified symbol/role map member is not a safe index: " +
        std::string(name));
  }
  return static_cast<std::size_t>(number);
}

[[nodiscard]] bool require_map_bool(
    const CanonicalValue& value, std::string_view name) {
  const auto* flag = std::get_if<bool>(
      &require_map_member(value, name).storage());
  if (flag == nullptr) {
    detail::fail_retained_bundle(
        "verified symbol/role map member is not a boolean: " +
        std::string(name));
  }
  return *flag;
}

[[nodiscard]] std::vector<std::string> require_map_text_array(
    const CanonicalValue& value, std::string_view name) {
  std::vector<std::string> result;
  const auto& rows = require_map_array(value, name);
  result.reserve(rows.size());
  for (const auto& row : rows) {
    const auto* text = std::get_if<std::string>(&row.storage());
    if (text == nullptr) {
      detail::fail_retained_bundle(
          "verified symbol/role map text array contains a non-text row: " +
          std::string(name));
    }
    result.push_back(*text);
  }
  return result;
}

[[nodiscard]] OccurrenceSymbolKind parse_symbol_kind(
    std::string_view value) {
  if (value == "model") {
    return OccurrenceSymbolKind::kModel;
  }
  if (value == "robot") {
    return OccurrenceSymbolKind::kRobot;
  }
  if (value == "material") {
    return OccurrenceSymbolKind::kMaterial;
  }
  if (value == "link") {
    return OccurrenceSymbolKind::kLink;
  }
  if (value == "joint") {
    return OccurrenceSymbolKind::kJoint;
  }
  if (value == "visual") {
    return OccurrenceSymbolKind::kVisual;
  }
  if (value == "collision") {
    return OccurrenceSymbolKind::kCollision;
  }
  if (value == "srdf_group") {
    return OccurrenceSymbolKind::kSrdfGroup;
  }
  if (value == "srdf_group_state") {
    return OccurrenceSymbolKind::kSrdfGroupState;
  }
  if (value == "srdf_end_effector") {
    return OccurrenceSymbolKind::kSrdfEndEffector;
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown symbol kind");
}

[[nodiscard]] OccurrenceReferenceKind parse_reference_kind(
    std::string_view value) {
  if (value == "joint_parent_link") {
    return OccurrenceReferenceKind::kJointParentLink;
  }
  if (value == "joint_child_link") {
    return OccurrenceReferenceKind::kJointChildLink;
  }
  if (value == "joint_mimic") {
    return OccurrenceReferenceKind::kJointMimic;
  }
  if (value == "visual_material") {
    return OccurrenceReferenceKind::kVisualMaterial;
  }
  if (value == "srdf_group_link") {
    return OccurrenceReferenceKind::kSrdfGroupLink;
  }
  if (value == "srdf_group_joint") {
    return OccurrenceReferenceKind::kSrdfGroupJoint;
  }
  if (value == "srdf_chain_base_link") {
    return OccurrenceReferenceKind::kSrdfChainBaseLink;
  }
  if (value == "srdf_chain_tip_link") {
    return OccurrenceReferenceKind::kSrdfChainTipLink;
  }
  if (value == "srdf_subgroup") {
    return OccurrenceReferenceKind::kSrdfSubgroup;
  }
  if (value == "srdf_group_state_group") {
    return OccurrenceReferenceKind::kSrdfGroupStateGroup;
  }
  if (value == "srdf_group_state_joint") {
    return OccurrenceReferenceKind::kSrdfGroupStateJoint;
  }
  if (value == "srdf_end_effector_parent_link") {
    return OccurrenceReferenceKind::kSrdfEndEffectorParentLink;
  }
  if (value == "srdf_end_effector_group") {
    return OccurrenceReferenceKind::kSrdfEndEffectorGroup;
  }
  if (value == "srdf_end_effector_parent_group") {
    return OccurrenceReferenceKind::kSrdfEndEffectorParentGroup;
  }
  if (value == "srdf_passive_joint") {
    return OccurrenceReferenceKind::kSrdfPassiveJoint;
  }
  if (value == "srdf_disabled_collision_first_link") {
    return OccurrenceReferenceKind::kSrdfDisabledCollisionFirstLink;
  }
  if (value == "srdf_disabled_collision_second_link") {
    return OccurrenceReferenceKind::kSrdfDisabledCollisionSecondLink;
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown reference kind");
}

[[nodiscard]] UrdfJointType parse_joint_type(std::string_view value) {
  if (value == "revolute") {
    return UrdfJointType::kRevolute;
  }
  if (value == "continuous") {
    return UrdfJointType::kContinuous;
  }
  if (value == "prismatic") {
    return UrdfJointType::kPrismatic;
  }
  if (value == "fixed") {
    return UrdfJointType::kFixed;
  }
  if (value == "floating") {
    return UrdfJointType::kFloating;
  }
  if (value == "planar") {
    return UrdfJointType::kPlanar;
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown URDF joint type");
}

[[nodiscard]] UrdfCoordinateKind parse_coordinate_kind(
    std::string_view value) {
  if (value == "none") {
    return UrdfCoordinateKind::kNone;
  }
  if (value == "angular") {
    return UrdfCoordinateKind::kAngular;
  }
  if (value == "linear") {
    return UrdfCoordinateKind::kLinear;
  }
  if (value == "planar") {
    return UrdfCoordinateKind::kPlanar;
  }
  if (value == "floating") {
    return UrdfCoordinateKind::kFloating;
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown coordinate kind");
}

[[nodiscard]] CompositionCoordinateUnit parse_coordinate_unit(
    std::string_view value) {
  if (value == "none") {
    return CompositionCoordinateUnit::kNone;
  }
  if (value == "radians") {
    return CompositionCoordinateUnit::kRadians;
  }
  if (value == "meters") {
    return CompositionCoordinateUnit::kMeters;
  }
  if (value == "planar_meters_radians") {
    return CompositionCoordinateUnit::kPlanarMetersRadians;
  }
  if (value == "floating_meters_radians") {
    return CompositionCoordinateUnit::kFloatingMetersRadians;
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown coordinate unit");
}

[[nodiscard]] CompositionResolvedFrame decode_resolved_frame(
    const CanonicalValue& value) {
  const std::string& kind = require_map_text(value, "kind");
  if (kind == "cell") {
    return CompositionCellFrame{
        core::CellId(require_map_text(value, "cell_id")),
        require_map_text(value, "compiled_frame_name")};
  }
  if (kind == "component_model") {
    return CompositionComponentModelFrame{
        core::ControllableComponentId(
            require_map_text(value, "controllable_component_id")),
        core::ModelInstanceId(
            require_map_text(value, "model_instance_id")),
        core::ModelRevision(require_map_text(value, "model_revision")),
        core::FrameRoleId(require_map_text(value, "frame_role_id")),
        require_map_text(value, "compiled_frame_name")};
  }
  if (kind == "passive_model") {
    return CompositionPassiveModelFrame{
        core::ModelInstanceId(
            require_map_text(value, "model_instance_id")),
        core::ModelRevision(require_map_text(value, "model_revision")),
        core::FrameRoleId(require_map_text(value, "frame_role_id")),
        require_map_text(value, "compiled_frame_name")};
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown resolved-frame kind");
}

struct DecodedRetainedModelMap final {
  CompositionCellFrame cell_frame;
  std::vector<CompositionModelOccurrence> model_occurrences;
  std::vector<OccurrenceSymbolRewrite> symbols;
  std::vector<OccurrenceReferenceRewrite> references;
  std::vector<CompositionModelCoordinate> coordinates;
  std::vector<CompositionControlBindingCoordinateMap> control_bindings;
  std::vector<CompositionTargetResolution> targets;
};

[[nodiscard]] DecodedRetainedModelMap decode_retained_model_map(
    const CanonicalValue& map) {
  DecodedRetainedModelMap result{
      CompositionCellFrame{
          core::CellId(require_map_text(map, "cell_id")),
          require_map_text(require_map_member(map, "cell_frame"),
                           "compiled_frame_name")},
      {}, {}, {}, {}, {}, {}};

  const auto& occurrences = require_map_array(map, "model_occurrences");
  result.model_occurrences.reserve(occurrences.size());
  for (const auto& row : occurrences) {
    const auto component = require_map_optional_text(row, "owner_component_id");
    const auto role = require_map_optional_text(row, "owner_model_role_id");
    result.model_occurrences.push_back(CompositionModelOccurrence{
        core::ModelInstanceId(require_map_text(row, "model_instance_id")),
        core::ModelAssetId(require_map_text(row, "model_asset_id")),
        core::ModelRevision(require_map_text(row, "model_revision")),
        component.has_value()
            ? std::optional<core::ControllableComponentId>(
                  core::ControllableComponentId(*component))
            : std::nullopt,
        role.has_value()
            ? std::optional<core::ModelRoleId>(core::ModelRoleId(*role))
            : std::nullopt,
        require_map_text(row, "compiled_model_name"),
        require_map_text(row, "compiled_robot_name"),
        require_map_text(row, "compiled_root_link_name")});
  }

  const auto& symbols = require_map_array(map, "symbol_rows");
  result.symbols.reserve(symbols.size());
  for (const auto& row : symbols) {
    result.symbols.push_back(OccurrenceSymbolRewrite{
        OccurrenceSymbolKey{
            core::ModelInstanceId(
                require_map_text(row, "model_instance_id")),
            parse_symbol_kind(require_map_text(row, "kind")),
            require_map_text(row, "scope_name"),
            require_map_text(row, "source_name")},
        require_map_text(row, "compiled_name")});
  }

  const auto& references = require_map_array(map, "reference_rows");
  result.references.reserve(references.size());
  for (const auto& row : references) {
    result.references.push_back(OccurrenceReferenceRewrite{
        core::ModelInstanceId(require_map_text(row, "model_instance_id")),
        parse_reference_kind(require_map_text(row, "kind")),
        require_map_text(row, "owner_scope_name"),
        require_map_text(row, "owner_source_name"),
        require_map_index(row, "reference_ordinal"),
        OccurrenceSymbolKey{
            core::ModelInstanceId(
                require_map_text(row, "target_model_instance_id")),
            parse_symbol_kind(require_map_text(row, "target_kind")),
            require_map_text(row, "target_scope_name"),
            require_map_text(row, "target_source_name")},
        require_map_text(row, "compiled_target_name")});
  }

  const auto& coordinates = require_map_array(map, "coordinate_rows");
  result.coordinates.reserve(coordinates.size());
  for (const auto& row : coordinates) {
    const auto component = require_map_optional_text(row, "owner_component_id");
    result.coordinates.push_back(CompositionModelCoordinate{
        core::ModelInstanceId(require_map_text(row, "model_instance_id")),
        component.has_value()
            ? std::optional<core::ControllableComponentId>(
                  core::ControllableComponentId(*component))
            : std::nullopt,
        require_map_index(row, "source_joint_index"),
        require_map_text(row, "source_joint_name"),
        require_map_text(row, "compiled_joint_name"),
        parse_joint_type(require_map_text(row, "joint_type")),
        parse_coordinate_kind(require_map_text(row, "coordinate_kind")),
        parse_coordinate_unit(require_map_text(row, "unit")),
        require_map_optional_text(row, "mimic_source_compiled_joint_name"),
        require_map_number(row, "mimic_multiplier"),
        require_map_number(row, "mimic_offset")});
  }

  const auto& bindings =
      require_map_array(map, "control_binding_coordinate_maps");
  result.control_bindings.reserve(bindings.size());
  for (const auto& row : bindings) {
    std::vector<CompositionResourceCoordinateRow> resources;
    const auto& resource_rows = require_map_array(row, "resources");
    resources.reserve(resource_rows.size());
    for (const auto& resource : resource_rows) {
      std::vector<CompositionCoordinateProjection> projections;
      const auto& projection_rows =
          require_map_array(resource, "coordinates");
      projections.reserve(projection_rows.size());
      for (const auto& projection : projection_rows) {
        projections.push_back(CompositionCoordinateProjection{
            core::ModelInstanceId(
                require_map_text(projection, "model_instance_id")),
            require_map_text(projection, "source_joint_name"),
            require_map_text(projection, "compiled_joint_name"),
            parse_coordinate_kind(
                require_map_text(projection, "coordinate_kind")),
            parse_coordinate_unit(require_map_text(projection, "unit")),
            require_map_number(projection, "multiplier"),
            require_map_number(projection, "offset"),
            require_map_bool(projection, "derived_mimic")});
      }
      resources.push_back(CompositionResourceCoordinateRow{
          core::ResourceRoleId(
              require_map_text(resource, "resource_role_id")),
          core::ResourceId(require_map_text(resource, "resource_id")),
          std::move(projections)});
    }
    result.control_bindings.push_back(
        CompositionControlBindingCoordinateMap{
            core::ControlBindingId(
                require_map_text(row, "control_binding_id")),
            core::ControllableComponentId(
                require_map_text(row, "controllable_component_id")),
            core::CommandTargetId(
                require_map_text(row, "command_target_id")),
            std::move(resources)});
  }

  const auto& targets = require_map_array(map, "target_role_maps");
  result.targets.reserve(targets.size());
  for (const auto& row : targets) {
    std::vector<CompositionTargetGroupResolution> groups;
    const auto& group_rows = require_map_array(row, "model_groups");
    groups.reserve(group_rows.size());
    for (const auto& group : group_rows) {
      groups.push_back(CompositionTargetGroupResolution{
          core::ModelGroupRoleId(
              require_map_text(group, "model_group_role_id")),
          core::ModelInstanceId(
              require_map_text(group, "model_instance_id")),
          require_map_text(group, "compiled_group_name"),
          require_map_text_array(group, "expanded_compiled_link_names"),
          require_map_text_array(group, "expanded_compiled_joint_names")});
    }
    std::vector<CompositionTargetFrameResolution> frames;
    const auto& frame_rows = require_map_array(row, "frames");
    frames.reserve(frame_rows.size());
    for (const auto& frame : frame_rows) {
      frames.push_back(CompositionTargetFrameResolution{
          core::FrameRoleId(require_map_text(frame, "frame_role_id")),
          decode_resolved_frame(require_map_member(frame, "frame"))});
    }
    std::vector<CompositionTargetCollisionResolution> collisions;
    const auto& collision_rows = require_map_array(row, "collision_bodies");
    collisions.reserve(collision_rows.size());
    for (const auto& collision : collision_rows) {
      collisions.push_back(CompositionTargetCollisionResolution{
          core::CollisionBodyRoleId(
              require_map_text(collision, "collision_body_role_id")),
          core::ModelInstanceId(
              require_map_text(collision, "model_instance_id")),
          require_map_text_array(collision, "compiled_link_names")});
    }
    result.targets.push_back(CompositionTargetResolution{
        core::ControllableComponentId(
            require_map_text(row, "controllable_component_id")),
        core::CommandTargetId(require_map_text(row, "command_target_id")),
        std::move(groups), std::move(frames), std::move(collisions)});
  }
  return result;
}

void require_symbol_role_map_matches_retained_graph(
    const CanonicalValue& map, const core::CellGraphDTO& graph) {
  if (require_map_text(map, "cell_id") != graph.cell_id.value()) {
    detail::fail_retained_bundle(
        "retained symbol/role map belongs to another cell");
  }

  using Owner = std::pair<std::string, std::string>;
  std::map<std::string, Owner, std::less<>> owners;
  for (const auto& component : graph.controllable_components) {
    for (const auto& binding : component.model_role_bindings) {
      if (!owners
               .emplace(binding.model_instance_id.value(),
                        Owner{component.controllable_component_id.value(),
                              binding.model_role_id.value()})
               .second) {
        detail::fail_retained_bundle(
            "retained graph gives one model occurrence multiple owners");
      }
    }
  }
  using ModelIdentity =
      std::tuple<std::string, std::string, std::optional<Owner>>;
  std::map<std::string, ModelIdentity, std::less<>> expected_models;
  for (const auto& model : graph.model_instances) {
    const auto owner = owners.find(model.model_instance_id.value());
    const std::optional<Owner> selected_owner =
        owner == owners.end() ? std::nullopt
                              : std::optional<Owner>(owner->second);
    expected_models.emplace(
        model.model_instance_id.value(),
        ModelIdentity{model.model_asset_id.value(),
                      model.model_revision.value(), selected_owner});
  }
  std::set<std::string, std::less<>> found_models;
  for (const auto& row : require_map_array(map, "model_occurrences")) {
    const std::string& instance = require_map_text(row, "model_instance_id");
    const auto expected = expected_models.find(instance);
    if (expected == expected_models.end() ||
        !found_models.insert(instance).second ||
        require_map_text(row, "model_asset_id") !=
            std::get<0>(expected->second) ||
        require_map_text(row, "model_revision") !=
            std::get<1>(expected->second)) {
      detail::fail_retained_bundle(
          "retained symbol/role map model occurrence differs from the "
          "retained graph");
    }
    const auto component =
        require_map_optional_text(row, "owner_component_id");
    const auto role = require_map_optional_text(row, "owner_model_role_id");
    const auto& expected_owner = std::get<2>(expected->second);
    if ((!expected_owner.has_value() &&
         (component.has_value() || role.has_value())) ||
        (expected_owner.has_value() &&
         (component != expected_owner->first ||
          role != expected_owner->second))) {
      detail::fail_retained_bundle(
          "retained symbol/role map model owner differs from the retained "
          "component graph");
    }
  }
  if (found_models.size() != expected_models.size()) {
    detail::fail_retained_bundle(
        "retained symbol/role map omits a graph model occurrence");
  }

  using ResourceKey = std::pair<std::string, std::string>;
  using BindingIdentity =
      std::tuple<std::string, std::string, std::set<ResourceKey>>;
  std::map<std::string, BindingIdentity, std::less<>> expected_bindings;
  std::set<std::pair<std::string, std::string>> expected_targets;
  for (const auto& binding : graph.control_bindings) {
    std::set<ResourceKey> resources;
    for (const auto& resource : binding.resource_bindings) {
      resources.emplace(resource.resource_role_id.value(),
                        resource.resource_id.value());
    }
    if (!expected_bindings
             .emplace(
                 binding.control_binding_id.value(),
                 BindingIdentity{binding.controllable_component_id.value(),
                                 binding.command_target_id.value(),
                                 std::move(resources)})
             .second) {
      detail::fail_retained_bundle(
          "retained graph contains duplicate control-binding identities");
    }
    expected_targets.emplace(binding.controllable_component_id.value(),
                             binding.command_target_id.value());
  }
  std::set<std::string, std::less<>> found_bindings;
  for (const auto& row :
       require_map_array(map, "control_binding_coordinate_maps")) {
    const std::string& binding_id =
        require_map_text(row, "control_binding_id");
    const auto expected = expected_bindings.find(binding_id);
    std::set<ResourceKey> resources;
    for (const auto& resource : require_map_array(row, "resources")) {
      resources.emplace(require_map_text(resource, "resource_role_id"),
                        require_map_text(resource, "resource_id"));
    }
    if (expected == expected_bindings.end() ||
        !found_bindings.insert(binding_id).second ||
        require_map_text(row, "controllable_component_id") !=
            std::get<0>(expected->second) ||
        require_map_text(row, "command_target_id") !=
            std::get<1>(expected->second) ||
        resources != std::get<2>(expected->second)) {
      detail::fail_retained_bundle(
          "retained symbol/role map binding/resources differ from the "
          "retained graph");
    }
  }
  if (found_bindings.size() != expected_bindings.size()) {
    detail::fail_retained_bundle(
        "retained symbol/role map omits a graph control binding");
  }

  std::set<std::pair<std::string, std::string>> found_targets;
  for (const auto& row : require_map_array(map, "target_role_maps")) {
    found_targets.emplace(
        require_map_text(row, "controllable_component_id"),
        require_map_text(row, "command_target_id"));
  }
  if (found_targets != expected_targets) {
    detail::fail_retained_bundle(
        "retained symbol/role map targets differ from the retained graph");
  }
}

void require_verified_product_artifacts(
    const std::filesystem::path &raw_data,
    const core::RetainedProductDTO &product,
    const core::RetainedBundleDTO &bundle,
    const core::ControlDomainId *control_domain_subject = nullptr,
    const core::RuntimeUnitId* runtime_unit_subject = nullptr) {
  std::optional<std::string> runtime_manifest;
  std::optional<std::string> runtime_launchable;
  for (const auto &artifact : product.artifact_refs) {
    const std::string bytes = read_verified_raw_reference(
        raw_data, artifact.content_addressed_store_key, artifact.content_digest,
        artifact.byte_size, "retained product artifact reference");
    if (product.product_kind == core::RetainedProductKind::kSymbolRoleMap) {
      if (artifact.artifact_role_id.value() !=
              detail::kComposedModelSymbolRoleMapArtifactRole ||
          artifact.media_type != detail::kComposedModelSymbolRoleMapMediaType) {
        detail::fail_retained_bundle(
            "symbol/role-map product has an unexpected artifact role");
      }
      const CanonicalValue map =
          detail::parse_and_verify_composed_model_symbol_role_map(bytes);
      require_symbol_role_map_matches_retained_graph(map, bundle.cell_graph);
    } else if (product.product_kind ==
               core::RetainedProductKind::kControlDomainControl) {
      if (artifact.artifact_role_id.value() !=
              detail::kControlDomainControlArtifactRole ||
          artifact.media_type != detail::kControlDomainControlMediaType) {
        detail::fail_retained_bundle(
            "control-domain product has an unexpected artifact role");
      }
      if (control_domain_subject == nullptr) {
        detail::fail_retained_bundle(
            "control-domain product is missing its envelope subject");
      }
      detail::require_control_domain_control_matches_retained_bundle(
          bytes, bundle, *control_domain_subject);
    } else if (product.product_kind ==
               core::RetainedProductKind::kRuntimeUnitConfiguration) {
      if (runtime_unit_subject == nullptr) {
        detail::fail_retained_bundle("runtime-unit product is missing its envelope subject");
      }
      if (artifact.artifact_role_id.value() ==
          detail::kReferenceRuntimeManifestArtifactRole) {
        if (artifact.media_type != detail::kReferenceRuntimeManifestMediaType ||
            runtime_manifest.has_value()) {
          detail::fail_retained_bundle(
              "runtime-unit product has an incoherent manifest artifact");
        }
        runtime_manifest = bytes;
      } else if (artifact.artifact_role_id.value() ==
                 detail::kReferenceRuntimeLaunchableArtifactRole) {
        if (artifact.media_type != detail::kReferenceRuntimeLaunchableMediaType ||
            runtime_launchable.has_value()) {
          detail::fail_retained_bundle(
              "runtime-unit product has an incoherent launchable artifact");
        }
        runtime_launchable = bytes;
      } else {
        detail::fail_retained_bundle("runtime-unit product has an unexpected artifact role");
      }
    }
  }
  if (product.product_kind ==
      core::RetainedProductKind::kRuntimeUnitConfiguration) {
    if (!runtime_manifest.has_value() || !runtime_launchable.has_value()) {
      detail::fail_retained_bundle(
          "runtime-unit product omits its manifest or launchable artifact");
    }
    detail::require_reference_runtime_product_matches_retained_bundle(
        *runtime_manifest, *runtime_launchable, bundle, *runtime_unit_subject);
  }
}

void require_verified_bundle_references(
    const std::filesystem::path& trusted_root,
    const std::filesystem::path& raw_data, const RetainedBundle& bundle) {
  const auto& value = bundle.value();
  if (!value.catalogs.records.empty()) {
    const std::filesystem::path catalog_data =
        require_read_only_catalog_data(trusted_root);
    for (const auto& record : value.catalogs.records) {
      std::visit(
          [&catalog_data](const auto& typed_record) {
            static_cast<void>(read_verified_catalog_reference(
                catalog_data, typed_record.content_addressed_store_key,
                typed_record.content_digest, std::nullopt,
                "retained catalog-record reference"));
          },
          record);
    }
  }
  for (const auto& schema : value.catalogs.schema_artifacts) {
    static_cast<void>(read_verified_raw_reference(
        raw_data, schema.content_addressed_store_key,
        schema.content_digest, std::nullopt,
        "retained schema-artifact reference"));
  }

  const auto& products = value.compiled_products;
  require_verified_product_artifacts(raw_data,
                                     products.planning_collision_model, value);
  if (products.viewer_model.has_value()) {
    require_verified_product_artifacts(raw_data, *products.viewer_model, value);
  }
  for (const auto &group : products.control_domain_products) {
    for (const auto &product : group.products) {
      require_verified_product_artifacts(raw_data, product, value,
                                         &group.control_domain_id);
    }
  }
  for (const auto &product : products.symbol_role_maps) {
    require_verified_product_artifacts(raw_data, product, value);
  }
  for (const auto &group : products.runtime_unit_products) {
    for (const auto &product : group.products) {
      require_verified_product_artifacts(
          raw_data, product, value, nullptr, &group.runtime_unit_id);
    }
  }
}

[[nodiscard]] const core::RetainedProductDTO*
find_exact_product(const core::RetainedBundleDTO& bundle,
                   const RetainedProductArtifactQuery& query) {
  const core::RetainedProductDTO* match = nullptr;
  const auto consider =
      [&query, &match](const core::RetainedProductDTO& candidate) {
        if (candidate.product_id != query.product_id ||
            candidate.product_kind != query.product_kind) {
          return;
        }
        if (match != nullptr) {
          detail::fail_retained_bundle(
              "verified retained bundle contains duplicate exact product "
              "coordinates");
        }
        match = &candidate;
      };

  const auto& products = bundle.compiled_products;
  consider(products.planning_collision_model);
  if (products.viewer_model.has_value()) {
    consider(*products.viewer_model);
  }
  for (const auto& group : products.control_domain_products) {
    for (const auto& product : group.products) {
      consider(product);
    }
  }
  for (const auto& product : products.symbol_role_maps) {
    consider(product);
  }
  for (const auto& group : products.runtime_unit_products) {
    for (const auto& product : group.products) {
      consider(product);
    }
  }
  return match;
}

[[nodiscard]] std::filesystem::path require_unchanged_trusted_root(
    const std::filesystem::path& trusted_package_root,
    std::string_view capability_label) {
  const std::filesystem::path current_root =
      posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages);
  if (current_root != trusted_package_root) {
    detail::fail_retained_bundle(
        std::string(capability_label) +
        " trusted root changed after construction");
  }
  return current_root;
}

[[nodiscard]] const core::RetainedArtifactRefDTO&
require_exact_artifact_role(const core::RetainedProductDTO& product,
                            std::string_view role) {
  const core::RetainedArtifactRefDTO* match = nullptr;
  for (const auto& artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() != role) {
      continue;
    }
    if (match != nullptr) {
      detail::fail_retained_bundle(
          "retained planning product contains a duplicate artifact role");
    }
    match = &artifact;
  }
  if (match == nullptr) {
    detail::fail_retained_bundle(
        "retained planning product is missing a required artifact role");
  }
  return *match;
}

using ModelIdentity = std::pair<std::string, std::string>;

[[nodiscard]] std::set<ModelIdentity>
graph_selected_model_identities(const core::CellGraphDTO& graph) {
  std::set<ModelIdentity> selected;
  for (const auto& instance : graph.model_instances) {
    selected.emplace(instance.model_asset_id.value(),
                     instance.model_revision.value());
  }
  return selected;
}

[[nodiscard]] std::map<
    ModelIdentity, const core::ModelAssetCatalogRecordRefDTO*>
require_exact_selected_model_records(
    const core::RetainedBundleDTO& bundle,
    const std::set<ModelIdentity>& selected) {
  std::map<ModelIdentity,
           const core::ModelAssetCatalogRecordRefDTO*>
      records;
  for (const auto& record : bundle.catalogs.records) {
    const auto* model =
        std::get_if<core::ModelAssetCatalogRecordRefDTO>(&record);
    if (model == nullptr) {
      continue;
    }
    const ModelIdentity identity{
        model->record_id.value(), model->record_revision.value()};
    if (!selected.contains(identity) ||
        !records.emplace(identity, model).second) {
      detail::fail_retained_bundle(
          "retained planning lookup found an unselected or duplicate "
          "model-asset catalog record");
    }
  }
  if (records.size() != selected.size()) {
    detail::fail_retained_bundle(
        "retained planning lookup is missing a graph-selected model asset");
  }

  std::set<ModelIdentity> source_records;
  for (const auto& source : bundle.provenance.source_records) {
    const auto* model =
        std::get_if<core::RetainedModelAssetSourceRecordDTO>(&source);
    if (model == nullptr) {
      continue;
    }
    const ModelIdentity identity{
        model->record_id.value(), model->revision.value()};
    if (!selected.contains(identity) ||
        !source_records.insert(identity).second) {
      detail::fail_retained_bundle(
          "retained planning lookup found unselected or duplicate source "
          "provenance");
    }
  }
  if (source_records != selected) {
    detail::fail_retained_bundle(
        "retained planning source provenance is not the exact selected "
        "model closure");
  }
  return records;
}

void checked_accumulate(std::uint64_t& total, std::uint64_t amount,
                        std::uint64_t limit,
                        std::string_view diagnostic) {
  if (amount > limit || total > limit - amount) {
    detail::fail_retained_bundle(diagnostic);
  }
  total += amount;
}

[[nodiscard]] QualifiedModelAsset requalify_model_asset(
    const core::ModelAssetCatalogRecordRefDTO& reference,
    std::string_view catalog_bytes) {
  try {
    return resolve_and_qualify_model_asset(
        reference, reference.content_addressed_store_key, catalog_bytes);
  } catch (const std::exception& error) {
    detail::fail_retained_bundle(
        std::string("retained model-asset catalog requalification failed: ") +
        error.what());
  }
}

[[nodiscard]] SourceVerifiedModelAsset reverify_model_source(
    const ModelSourceRetrievalPlan& plan,
    std::vector<ResolvedModelSourceArtifactCandidate> candidates) {
  try {
    return verify_model_source_closure(plan, std::move(candidates));
  } catch (const std::exception& error) {
    detail::fail_retained_bundle(
        std::string("retained model-source closure verification failed: ") +
        error.what());
  }
}

[[nodiscard]] ModelSourceRetrievalPlan
preflight_requalified_model_source(
    const QualifiedModelAsset& qualified) {
  try {
    return preflight_model_source_declaration(qualified);
  } catch (const std::exception& error) {
    detail::fail_retained_bundle(
        std::string("retained model-source declaration preflight failed: ") +
        error.what());
  }
}

[[nodiscard]] std::vector<VerifiedRetainedPlanningResource>
read_verified_planning_resources(
    const std::filesystem::path& trusted_root,
    const std::filesystem::path& raw_data,
    const core::RetainedBundleDTO& bundle,
    std::span<const detail::PlanningResourceMapRow> resource_map) {
  const std::set<ModelIdentity> selected =
      graph_selected_model_identities(bundle.cell_graph);
  const auto records =
      require_exact_selected_model_records(bundle, selected);
  if (records.empty() && resource_map.empty()) {
    return {};
  }
  std::map<std::tuple<std::string, std::string, std::string>,
           const detail::PlanningResourceMapRow*>
      mapping;
  for (const auto& row : resource_map) {
    const ModelIdentity identity{
        row.model_asset_id.value(), row.model_revision.value()};
    if (!selected.contains(identity) ||
        !mapping.emplace(
                    std::make_tuple(
                        identity.first, identity.second,
                        row.source_logical_uri),
                    &row)
             .second) {
      detail::fail_retained_bundle(
          "retained planning resource map contains an unselected or "
          "duplicate source identity");
    }
  }
  const std::filesystem::path catalog_data =
      require_read_only_catalog_data(trusted_root);

  std::map<std::string, VerifiedRetainedPlanningResource, std::less<>>
      resources;
  std::size_t complete_source_count = 0U;
  std::size_t mapped_source_count = 0U;
  std::uint64_t complete_catalog_bytes = 0U;
  std::uint64_t complete_source_bytes = 0U;
  for (const auto& [identity, reference] : records) {
    std::string catalog_bytes = read_verified_catalog_reference(
        catalog_data, reference->content_addressed_store_key,
        reference->content_digest, std::nullopt,
        "selected model-asset catalog referent");
    if (catalog_bytes.size() > kMaxArtifactEncodedBytes) {
      detail::fail_retained_bundle(
          "retained model-asset catalog exceeds the fixed artifact bound");
    }
    checked_accumulate(
        complete_catalog_bytes,
        static_cast<std::uint64_t>(catalog_bytes.size()),
        kMaxModelSourceClosureBytes,
        "retained model-asset catalogs exceed the global byte bound");
    const QualifiedModelAsset qualified =
        requalify_model_asset(*reference, catalog_bytes);
    const ModelSourceRetrievalPlan plan =
        preflight_requalified_model_source(qualified);

    if (plan.artifacts().size() >
            core::kMaxModelAssetCollectionItems -
                complete_source_count) {
      detail::fail_retained_bundle(
          "retained planning source closure exceeds the global artifact "
          "count bound");
    }
    complete_source_count += plan.artifacts().size();

    std::vector<ResolvedModelSourceArtifactCandidate> candidates;
    candidates.reserve(plan.artifacts().size());
    for (const auto& artifact : plan.artifacts()) {
      checked_accumulate(
          complete_source_bytes, artifact.byte_size,
          kMaxModelSourceClosureBytes,
          "retained planning source closure exceeds the global byte bound");
      const std::string reference_label =
          "selected model-source artifact " +
          artifact.logical_uri;
      std::string bytes = read_verified_raw_reference(
          raw_data, artifact.expected_store_key,
          artifact.content_digest, artifact.byte_size,
          reference_label);
      candidates.push_back(
          {artifact.logical_uri, artifact.expected_store_key,
           std::move(bytes)});
    }
    const SourceVerifiedModelAsset verified =
        reverify_model_source(plan, std::move(candidates));
    if (verified.qualified_asset().reference() != *reference ||
        verified.value() != qualified.value() ||
        verified.verified_artifacts().size() !=
            plan.artifacts().size()) {
      detail::fail_retained_bundle(
          "retained model-source verification changed the selected "
          "catalog/source identity");
    }

    for (std::size_t index = 0U; index < plan.artifacts().size(); ++index) {
      const auto& artifact = plan.artifacts()[index];
      const auto& proof = verified.verified_artifacts()[index];
      if (proof.logical_uri != artifact.logical_uri ||
          proof.media_type != artifact.media_type ||
          proof.byte_size != artifact.byte_size ||
          proof.content_digest != artifact.content_digest ||
          proof.resolved_store_key != artifact.expected_store_key) {
        detail::fail_retained_bundle(
            "retained model-source proof differs from its exact retrieval "
            "plan");
      }
      if (artifact.artifact_kind ==
              ModelSourceArtifactKind::kUrdfXmlEntrypoint ||
          artifact.artifact_kind ==
              ModelSourceArtifactKind::kSrdfXmlEntrypoint) {
        continue;
      }

      const auto mapping_row = mapping.find(
          std::make_tuple(identity.first, identity.second,
                          artifact.logical_uri));
      if (mapping_row == mapping.end()) {
        detail::fail_retained_bundle(
            "retained planning resource map is missing a selected source "
            "resource");
      }
      const auto& mapped = *mapping_row->second;
      if (mapped.media_type != artifact.media_type ||
          mapped.byte_size != artifact.byte_size ||
          mapped.content_digest != artifact.content_digest ||
          mapped.content_addressed_store_key !=
              artifact.expected_store_key) {
        detail::fail_retained_bundle(
            "retained planning resource map differs from the verified "
            "source resource");
      }
      ++mapped_source_count;

      std::string bytes = read_verified_raw_reference(
          raw_data, artifact.expected_store_key,
          artifact.content_digest, artifact.byte_size,
          "selected non-XML planning resource " +
              artifact.logical_uri);
      VerifiedRetainedPlanningResource resource =
          detail::VerifiedRetainedPlanningConstructionAuthority::resource(
              mapped.compiled_logical_uri, artifact.media_type,
              artifact.byte_size, artifact.content_digest,
              artifact.expected_store_key, std::move(bytes));
      if (!resources.emplace(mapped.compiled_logical_uri,
                             std::move(resource)).second) {
        detail::fail_retained_bundle(
            "retained planning resource map repeats one compiled URI");
      }
    }
  }
  if (mapped_source_count != mapping.size()) {
    detail::fail_retained_bundle(
        "retained planning resource map contains an unused source resource");
  }

  std::vector<VerifiedRetainedPlanningResource> result;
  result.reserve(resources.size());
  for (auto& [unused, resource] : resources) {
    (void)unused;
    result.push_back(std::move(resource));
  }
  return result;
}

}  // namespace

const CompositionTargetResolution* VerifiedRetainedModelMap::find_target(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& command_target_id) const noexcept {
  const auto found = std::find_if(
      targets_.begin(), targets_.end(),
      [&component_id, &command_target_id](
          const CompositionTargetResolution& candidate) {
        return candidate.controllable_component_id == component_id &&
               candidate.command_target_id == command_target_id;
      });
  return found == targets_.end() ? nullptr : &*found;
}

const CompositionControlBindingCoordinateMap*
VerifiedRetainedModelMap::find_binding(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& command_target_id) const noexcept {
  const CompositionControlBindingCoordinateMap* match = nullptr;
  for (const auto& candidate : control_bindings_) {
    if (candidate.controllable_component_id != component_id ||
        candidate.command_target_id != command_target_id) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &candidate;
  }
  return match;
}

void VerifiedRetainedModelMap::require_same_bundle_as(
    const VerifiedRetainedPlanningModel& planning_model) const {
  if (query_.retained_bundle_id !=
      planning_model.query().retained_bundle_id) {
    detail::fail_retained_bundle(
        "retained model map and planning model belong to different bundles");
  }
}

FilesystemVerifiedRetainedBundleSource::
    FilesystemVerifiedRetainedBundleSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
}

std::shared_ptr<const RetainedBundle>
FilesystemVerifiedRetainedBundleSource::get_verified(
    const core::RetainedBundleId& retained_bundle_id) const {
  const std::filesystem::path current_root =
      require_unchanged_trusted_root(
          trusted_package_root_, "read-only retained-bundle");
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle =
      read_verified_bundle_entry(paths.bundle_data, retained_bundle_id);
  if (bundle == nullptr) {
    return nullptr;
  }
  require_verified_bundle_references(
      current_root, paths.raw_data, *bundle);
  return bundle;
}

FilesystemVerifiedRetainedProductArtifactSource::
    FilesystemVerifiedRetainedProductArtifactSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
}

std::optional<VerifiedRetainedProductArtifact>
FilesystemVerifiedRetainedProductArtifactSource::get_verified(
    const RetainedProductArtifactQuery& query) const {
  const std::filesystem::path current_root =
      require_unchanged_trusted_root(
          trusted_package_root_, "read-only retained-product");
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle =
      read_verified_bundle_entry(paths.bundle_data,
                                 query.retained_bundle_id);
  if (bundle == nullptr) {
    return std::nullopt;
  }
  const core::RetainedProductDTO* product =
      find_exact_product(bundle->value(), query);
  if (product == nullptr) {
    return std::nullopt;
  }
  const auto artifact = std::find_if(
      product->artifact_refs.begin(), product->artifact_refs.end(),
      [&query](const core::RetainedArtifactRefDTO& candidate) {
        return candidate.artifact_role_id == query.artifact_role_id;
      });
  if (artifact == product->artifact_refs.end()) {
    return std::nullopt;
  }

  std::string bytes = read_verified_raw_reference(
      paths.raw_data, artifact->content_addressed_store_key,
      artifact->content_digest, artifact->byte_size,
      "selected retained product artifact");
  if (product->product_kind == core::RetainedProductKind::kSymbolRoleMap) {
    if (artifact->artifact_role_id.value() !=
            detail::kComposedModelSymbolRoleMapArtifactRole ||
        artifact->media_type !=
            detail::kComposedModelSymbolRoleMapMediaType) {
      detail::fail_retained_bundle(
          "selected symbol/role-map artifact has an incoherent contract");
    }
    const CanonicalValue map =
        detail::parse_and_verify_composed_model_symbol_role_map(bytes);
    require_symbol_role_map_matches_retained_graph(map,
                                                   bundle->value().cell_graph);
  } else if (product->product_kind ==
             core::RetainedProductKind::kControlDomainControl) {
    if (artifact->artifact_role_id.value() !=
            detail::kControlDomainControlArtifactRole ||
        artifact->media_type != detail::kControlDomainControlMediaType) {
      detail::fail_retained_bundle(
          "selected control-domain artifact has an incoherent contract");
    }
    const core::ControlDomainId *subject = nullptr;
    for (const auto &group :
         bundle->value().compiled_products.control_domain_products) {
      if (std::any_of(
              group.products.begin(), group.products.end(),
              [&](const auto &candidate) { return &candidate == product; })) {
        subject = &group.control_domain_id;
        break;
      }
    }
    if (subject == nullptr) {
      detail::fail_retained_bundle(
          "selected control-domain artifact lost its envelope subject");
    }
    detail::require_control_domain_control_matches_retained_bundle(
        bytes, bundle->value(), *subject);
  } else if (product->product_kind ==
             core::RetainedProductKind::kRuntimeUnitConfiguration) {
    const core::RuntimeUnitId* subject = nullptr;
    for (const auto& group :
         bundle->value().compiled_products.runtime_unit_products) {
      if (std::any_of(group.products.begin(), group.products.end(),
                      [&](const auto& candidate) { return &candidate == product; })) {
        subject = &group.runtime_unit_id;
        break;
      }
    }
    if (subject == nullptr) {
      detail::fail_retained_bundle(
          "selected runtime-unit artifact lost its envelope subject");
    }
    require_verified_product_artifacts(
        paths.raw_data, *product, bundle->value(), nullptr, subject);
  }
  return VerifiedRetainedProductArtifact(query, product->product_contract_id,
                                         product->product_contract_revision,
                                         *artifact, std::move(bytes));
}

std::optional<VerifiedRetainedRuntimeUnitProduct>
verify_reference_runtime_unit_product(
    const RetainedBundle& bundle, const core::RuntimeUnitId& runtime_unit_id,
    const IVerifiedRetainedProductArtifactSource& artifact_source) {
  if (!verify_retained_bundle_identity(bundle)) {
    detail::fail_retained_bundle(
        "runtime-unit verification requires one sealed valid bundle");
  }
  const auto& groups = bundle.value().compiled_products.runtime_unit_products;
  const auto group = std::find_if(
      groups.begin(), groups.end(), [&](const auto& candidate) {
        return candidate.runtime_unit_id == runtime_unit_id;
      });
  if (group == groups.end()) {
    return std::nullopt;
  }
  if (group->products.size() != 1U) {
    detail::fail_retained_bundle(
        "runtime-unit verification found an incoherent product group");
  }
  const auto& product = group->products.front();
  const RetainedProductArtifactQuery manifest_query{
      bundle.retained_bundle_id(),
      product.product_id,
      core::RetainedProductKind::kRuntimeUnitConfiguration,
      core::ArtifactRoleId(
          std::string(detail::kReferenceRuntimeManifestArtifactRole)),
  };
  const RetainedProductArtifactQuery launchable_query{
      bundle.retained_bundle_id(),
      product.product_id,
      core::RetainedProductKind::kRuntimeUnitConfiguration,
      core::ArtifactRoleId(
          std::string(detail::kReferenceRuntimeLaunchableArtifactRole)),
  };
  const auto manifest = artifact_source.get_verified(manifest_query);
  const auto launchable = artifact_source.get_verified(launchable_query);
  if (!manifest.has_value() || !launchable.has_value() ||
      manifest->query() != manifest_query ||
      launchable->query() != launchable_query ||
      manifest->product_contract_id() != product.product_contract_id ||
      launchable->product_contract_id() != product.product_contract_id ||
      manifest->product_contract_revision() != product.product_contract_revision ||
      launchable->product_contract_revision() != product.product_contract_revision) {
    detail::fail_retained_bundle(
        "runtime-unit artifact source did not return the exact product pair");
  }
  detail::require_reference_runtime_product_matches_retained_bundle(
      manifest->bytes(), launchable->bytes(), bundle.value(), runtime_unit_id);
  return VerifiedRetainedRuntimeUnitProduct(
      bundle.retained_bundle_id(), runtime_unit_id, product.product_id,
      detail::retained_product_output_digest(product),
      manifest->artifact_ref().content_digest, product.product_contract_id,
      product.product_contract_revision, launchable->artifact_ref(),
      std::string(launchable->bytes()),
      std::string(detail::kReferenceRuntimeOperatingSystem),
      std::string(detail::kReferenceRuntimeShellContract),
      std::string(detail::kReferenceRuntimeRoleId),
      std::string(detail::kReferenceRuntimeStatusEndpointRoleId),
      std::string(detail::kReferenceRuntimeStatusEndpointContractId),
      detail::reference_runtime_status_endpoint_revision(), "stdout_line",
      std::string(detail::kReferenceRuntimeProcessAdapterContractId),
      detail::reference_runtime_process_adapter_revision(),
      std::string(detail::kReferenceRuntimeRetrievalContractId),
      detail::reference_runtime_retrieval_contract_revision());
}

FilesystemVerifiedRetainedModelMapSource::
    FilesystemVerifiedRetainedModelMapSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
}

std::optional<VerifiedRetainedModelMap>
FilesystemVerifiedRetainedModelMapSource::get_verified(
    const RetainedModelMapQuery& query) const {
  const std::filesystem::path current_root =
      require_unchanged_trusted_root(
          trusted_package_root_, "read-only retained model-map");
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle = read_verified_bundle_entry(
      paths.bundle_data, query.retained_bundle_id);
  if (bundle == nullptr) {
    return std::nullopt;
  }

  const RetainedProductArtifactQuery exact_product_query{
      query.retained_bundle_id, query.product_id,
      core::RetainedProductKind::kSymbolRoleMap,
      core::ArtifactRoleId(
          std::string(detail::kComposedModelSymbolRoleMapArtifactRole))};
  const core::RetainedProductDTO* product =
      find_exact_product(bundle->value(), exact_product_query);
  if (product == nullptr) {
    return std::nullopt;
  }
  const core::RetainedArtifactRefDTO* selected = nullptr;
  for (const auto& artifact : product->artifact_refs) {
    if (artifact.artifact_role_id !=
        exact_product_query.artifact_role_id) {
      continue;
    }
    if (selected != nullptr) {
      detail::fail_retained_bundle(
          "retained model-map product repeats its required artifact role");
    }
    selected = &artifact;
  }
  if (selected == nullptr) {
    return std::nullopt;
  }
  if (selected->media_type !=
      detail::kComposedModelSymbolRoleMapMediaType) {
    detail::fail_retained_bundle(
        "retained model-map artifact has an incoherent media type");
  }

  const std::string bytes = read_verified_raw_reference(
      paths.raw_data, selected->content_addressed_store_key,
      selected->content_digest, selected->byte_size,
      "selected retained model-map artifact");
  const CanonicalValue parsed =
      detail::parse_and_verify_composed_model_symbol_role_map(bytes);
  require_symbol_role_map_matches_retained_graph(
      parsed, bundle->value().cell_graph);
  DecodedRetainedModelMap decoded = decode_retained_model_map(parsed);

  return VerifiedRetainedModelMap(
      query, product->product_contract_id,
      product->product_contract_revision, std::move(decoded.cell_frame),
      std::move(decoded.model_occurrences), std::move(decoded.symbols),
      std::move(decoded.references), std::move(decoded.coordinates),
      std::move(decoded.control_bindings), std::move(decoded.targets));
}

FilesystemVerifiedRetainedPlanningModelSource::
    FilesystemVerifiedRetainedPlanningModelSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
}

std::optional<VerifiedRetainedPlanningModel>
FilesystemVerifiedRetainedPlanningModelSource::get_verified(
    const RetainedPlanningModelQuery& query) const {
  const std::filesystem::path current_root =
      require_unchanged_trusted_root(
          trusted_package_root_, "read-only retained planning-model");
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle = read_verified_bundle_entry(
      paths.bundle_data, query.retained_bundle_id);
  if (bundle == nullptr) {
    return std::nullopt;
  }

  // This capability recognizes one closed placement, not any equal-spelled
  // product elsewhere in the retained product table.
  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  if (product.product_id != query.product_id) {
    return std::nullopt;
  }
  if (product.product_kind !=
      core::RetainedProductKind::kPlanningCollisionModel) {
    detail::fail_retained_bundle(
        "selected planning product has the wrong retained product kind");
  }

  // Verify every explicit retained reference before returning a narrower
  // projection, then independently retrieve the selected product roles.
  require_verified_bundle_references(
      current_root, paths.raw_data, *bundle);
  const auto& resource_map_ref = require_exact_artifact_role(
      product, detail::kPlanningResourceMapArtifactRole);
  const auto& urdf_ref = require_exact_artifact_role(product, "urdf");
  const auto& srdf_ref = require_exact_artifact_role(product, "srdf");
  const std::string resource_map_bytes = read_verified_raw_reference(
      paths.raw_data,
      resource_map_ref.content_addressed_store_key,
      resource_map_ref.content_digest, resource_map_ref.byte_size,
      "selected retained planning resource map");
  std::string urdf = read_verified_raw_reference(
      paths.raw_data, urdf_ref.content_addressed_store_key,
      urdf_ref.content_digest, urdf_ref.byte_size,
      "selected retained planning URDF");
  std::string srdf = read_verified_raw_reference(
      paths.raw_data, srdf_ref.content_addressed_store_key,
      srdf_ref.content_digest, srdf_ref.byte_size,
      "selected retained planning SRDF");
  if (urdf.empty() || srdf.empty() ||
      urdf.size() > kMaxModelSourceXmlEntrypointBytes ||
      srdf.size() > kMaxModelSourceXmlEntrypointBytes) {
      detail::fail_retained_bundle(
          "retained planning XML violates the fixed entrypoint bound");
  }

  const std::vector<detail::PlanningResourceMapRow> resource_map =
      detail::parse_and_verify_planning_resource_map(
          resource_map_bytes);
  std::vector<VerifiedRetainedPlanningResource> resources =
      read_verified_planning_resources(
          current_root, paths.raw_data, bundle->value(), resource_map);
  std::uint64_t result_bytes =
      static_cast<std::uint64_t>(urdf.size());
  checked_accumulate(
      result_bytes, static_cast<std::uint64_t>(srdf.size()),
      kMaxModelSourceClosureBytes,
      "retained planning result exceeds the global byte bound");
  for (const auto& resource : resources) {
    checked_accumulate(
        result_bytes, resource.byte_size(),
        kMaxModelSourceClosureBytes,
        "retained planning result exceeds the global byte bound");
  }

  return VerifiedRetainedPlanningModel(
      query, std::move(urdf), std::move(srdf),
      std::move(resources));
}

}  // namespace manyforge::deployment
