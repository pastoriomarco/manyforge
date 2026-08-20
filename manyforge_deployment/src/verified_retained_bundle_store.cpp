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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_artifact_store_internal.hpp"
#include "composed_model_symbol_role_map_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/control_interface_contract_codec.hpp"
#include "manyforge_deployment/controller_topology_codec.hpp"
#include "manyforge_deployment/model_asset_codec.hpp"
#include "manyforge_deployment/model_source.hpp"
#include "manyforge_deployment/program_binding.hpp"
#include "manyforge_deployment/program_contract.hpp"
#include "manyforge_deployment/provider_manifest_codec.hpp"
#include "manyforge_deployment/retained_bundle.hpp"
#include "manyforge_deployment/stop_evidence_policy_codec.hpp"
#include "native_execution_runtime_unit_internal.hpp"
#include "planning_resource_map_internal.hpp"
#include "posix_durability_internal.hpp"
#include "reference_runtime_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "stop_qualification_evidence_internal.hpp"
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

[[nodiscard]] std::optional<std::filesystem::path>
find_read_only_data_directory(
    const std::filesystem::path& trusted_root,
    const std::string_view namespace_name,
    const std::string_view data_directory_name) {
  const std::filesystem::path value_namespace =
      trusted_root / std::string(namespace_name);
  const std::filesystem::path value_data =
      value_namespace / std::string(data_directory_name);
  for (const auto& directory :
       {value_namespace, value_data}) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory, error);
    if (error == std::errc::no_such_file_or_directory) {
      return std::nullopt;
    }
    if (error) {
      throw std::filesystem::filesystem_error(
          std::string(kReadOnlyDirectoryMessages.inspect), directory, error);
    }
    if (!std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
      detail::fail_retained_bundle(
          kReadOnlyDirectoryMessages.not_existing_plain_directory);
    }
  }
  return value_data;
}

[[nodiscard]] std::filesystem::path require_read_only_catalog_data(
    const std::filesystem::path& trusted_root) {
  const auto data = find_read_only_data_directory(
      trusted_root, detail::kCatalogArtifactNamespaceDirectory,
      detail::kCatalogArtifactDataDirectory);
  if (!data.has_value()) {
    detail::fail_retained_bundle(
        "read-only catalog-artifact store is absent");
  }
  return *data;
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

[[nodiscard]] std::optional<std::string> read_verified_catalog_entry(
    const std::filesystem::path& catalog_data,
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& digest,
    const std::optional<std::uint64_t> expected_size,
    const std::uint64_t maximum_single_payload_bytes) {
  const auto storage_hex =
      detail::catalog_artifact_storage_hex(key.value());
  if (!storage_hex.has_value() ||
      !is_sha256_digest(digest.value())) {
    return std::nullopt;
  }
  if (maximum_single_payload_bytes == 0U ||
      maximum_single_payload_bytes >
          std::numeric_limits<std::uint64_t>::max() -
              detail::kMaximumCatalogArtifactEnvelopeOverhead) {
    throw std::invalid_argument(
        "read-only catalog-artifact byte bound is invalid");
  }
  const std::uint64_t maximum_envelope_bytes =
      maximum_single_payload_bytes +
      detail::kMaximumCatalogArtifactEnvelopeOverhead;
  auto envelope = posix::read_exact_regular_file(
      catalog_data /
          (std::string(*storage_hex) +
           std::string(detail::kCatalogArtifactFileSuffix)),
      maximum_envelope_bytes, true, kPosixErrors, kReadMessages);
  if (!envelope.has_value()) {
    return std::nullopt;
  }
  detail::DecodedCatalogArtifactEnvelope decoded =
      detail::decode_catalog_artifact_envelope(*envelope);
  if (decoded.content_addressed_store_key != key.value()) {
    detail::fail_retained_bundle(
        "durable catalog-artifact filename collision");
  }
  if (decoded.content_digest != digest.value() ||
      (expected_size.has_value() &&
       decoded.payload.size() != *expected_size)) {
    return std::nullopt;
  }
  return std::string(decoded.payload);
}

[[nodiscard]] std::string read_verified_catalog_reference(
    const std::filesystem::path& catalog_data,
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& digest,
    const std::optional<std::uint64_t> expected_size,
    const std::string_view reference_label) {
  auto bytes = read_verified_catalog_entry(
      catalog_data, key, digest, expected_size,
      kMaxDurableRetainedBlobBytes);
  if (!bytes.has_value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " is absent or failed exact durable catalog-artifact verification");
  }
  return std::move(*bytes);
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

[[nodiscard]] std::optional<core::CoordinateKind>
require_map_optional_coordinate_kind(const CanonicalValue& value,
                                     std::string_view name) {
  const auto kind = require_map_optional_text(value, name);
  if (!kind.has_value()) {
    return std::nullopt;
  }
  if (*kind == "angular") {
    return core::CoordinateKind::kAngular;
  }
  if (*kind == "linear") {
    return core::CoordinateKind::kLinear;
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown component-local coordinate "
      "kind");
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

[[nodiscard]] double require_verified_number(
    const CanonicalValue& value, std::string_view diagnostic) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    detail::fail_retained_bundle(std::string(diagnostic));
  }
  return *number;
}

[[nodiscard]] core::PlacementPoseDTO decode_placement_pose(
    const CanonicalValue& value) {
  const auto& position = require_map_array(value, "position_m");
  const auto& orientation = require_map_array(value, "orientation_xyzw");
  if (position.size() != 3U || orientation.size() != 4U) {
    detail::fail_retained_bundle(
        "verified symbol/role map placement has an invalid shape");
  }
  core::PlacementPoseDTO result;
  for (std::size_t index = 0U; index < position.size(); ++index) {
    result.position_m[index] = require_verified_number(
        position[index],
        "verified symbol/role map placement position is not finite");
  }
  for (std::size_t index = 0U; index < orientation.size(); ++index) {
    result.orientation_xyzw[index] = require_verified_number(
        orientation[index],
        "verified symbol/role map placement orientation is not finite");
  }
  return result;
}

[[nodiscard]] CompositionConnectionSource decode_connection_source(
    const CanonicalValue& value) {
  const std::string& kind = require_map_text(value, "kind");
  if (kind == "direct_model_anchor") {
    return CompositionDirectModelAnchor{
        core::ModelInstanceId(
            require_map_text(value, "model_instance_id")),
        {}};
  }
  if (kind == "direct_component_anchor") {
    return CompositionDirectComponentAnchor{
        core::ControllableComponentId(
            require_map_text(value, "controllable_component_id")),
        core::ModelInstanceId(
            require_map_text(value, "root_model_instance_id")),
        {}};
  }
  if (kind == "model_mount") {
    return CompositionModelMount{
        core::ControllableComponentId(
            require_map_text(value, "controllable_component_id")),
        core::ModelInstanceId(
            require_map_text(value, "parent_model_instance_id")),
        core::ModelInstanceId(
            require_map_text(value, "child_model_instance_id")),
        {}};
  }
  if (kind == "component_attachment") {
    return CompositionComponentAttachment{
        core::ComponentAttachmentId(
            require_map_text(value, "component_attachment_id")),
        core::ControllableComponentId(
            require_map_text(value, "parent_component_id")),
        core::ControllableComponentId(
            require_map_text(value, "child_component_id")),
        {}};
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown connection-source kind");
}

[[nodiscard]] CompositionFramePathSegment decode_frame_path_segment(
    const CanonicalValue& value) {
  const std::string& kind = require_map_text(value, "kind");
  if (kind == "fixed_connection") {
    return CompositionFixedPathSegment{
        require_map_index(value, "fixed_connection_index")};
  }
  if (kind == "model_joint") {
    return CompositionModelJointPathSegment{
        core::ModelInstanceId(
            require_map_text(value, "model_instance_id")),
        require_map_index(value, "composition_coordinate_index"),
        require_map_text(value, "source_joint_name"),
        require_map_text(value, "compiled_joint_name"),
        require_map_text(value, "compiled_parent_link_name"),
        require_map_text(value, "compiled_child_link_name")};
  }
  detail::fail_retained_bundle(
      "verified symbol/role map has an unknown frame-path segment kind");
}

struct DecodedRetainedModelMap final {
  CompositionCellFrame cell_frame;
  std::vector<CompositionModelOccurrence> model_occurrences;
  std::vector<CompositionModelCollisionBodyResolution>
      model_collision_bodies;
  std::vector<OccurrenceSymbolRewrite> symbols;
  std::vector<OccurrenceReferenceRewrite> references;
  std::vector<CompositionModelCoordinate> coordinates;
  std::vector<CompositionControlBindingCoordinateMap> control_bindings;
  std::vector<CompositionFixedConnection> fixed_connections;
  std::vector<CompositionFramePath> frame_paths;
  std::vector<CompositionTargetResolution> targets;
};

[[nodiscard]] DecodedRetainedModelMap decode_retained_model_map(
    const CanonicalValue& map) {
  DecodedRetainedModelMap result{
      CompositionCellFrame{
          core::CellId(require_map_text(map, "cell_id")),
          require_map_text(require_map_member(map, "cell_frame"),
                           "compiled_frame_name")},
      {}, {}, {}, {}, {}, {}, {}, {}, {}};

  const auto& occurrences = require_map_array(map, "model_occurrences");
  result.model_occurrences.reserve(occurrences.size());
  for (const auto& row : occurrences) {
    const auto component = require_map_optional_text(row, "owner_component_id");
    const auto role = require_map_optional_text(row, "owner_model_role_id");
    const core::ModelInstanceId model_instance_id(
        require_map_text(row, "model_instance_id"));
    const core::ModelRevision model_revision(
        require_map_text(row, "model_revision"));
    const std::optional<core::ControllableComponentId> owner_component_id =
        component.has_value()
            ? std::optional<core::ControllableComponentId>(
                  core::ControllableComponentId(*component))
            : std::nullopt;
    result.model_occurrences.push_back(CompositionModelOccurrence{
        model_instance_id,
        core::ModelAssetId(require_map_text(row, "model_asset_id")),
        model_revision,
        owner_component_id,
        role.has_value()
            ? std::optional<core::ModelRoleId>(core::ModelRoleId(*role))
            : std::nullopt,
        require_map_text(row, "compiled_model_name"),
        require_map_text(row, "compiled_robot_name"),
        require_map_text(row, "compiled_root_link_name")});

    const auto& collision_roles =
        require_map_array(row, "collision_body_roles");
    result.model_collision_bodies.reserve(
        result.model_collision_bodies.size() + collision_roles.size());
    for (const auto& collision : collision_roles) {
      result.model_collision_bodies.push_back(
          CompositionModelCollisionBodyResolution{
              model_instance_id, model_revision, owner_component_id,
              core::CollisionBodyRoleId(
                  require_map_text(collision, "collision_body_role_id")),
              require_map_text_array(collision, "compiled_link_names")});
    }
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
          std::move(projections),
          require_map_optional_coordinate_kind(
              resource, "component_local_coordinate_kind")});
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

  const auto& fixed_connections =
      require_map_array(map, "fixed_connections");
  result.fixed_connections.reserve(fixed_connections.size());
  for (const auto& row : fixed_connections) {
    const auto source = decode_connection_source(
        require_map_member(row, "source"));
    const auto pose = decode_placement_pose(
        require_map_member(row, "child_pose_in_parent"));
    result.fixed_connections.push_back(CompositionFixedConnection{
        std::visit(
            [&pose](auto value) -> CompositionConnectionSource {
              using Source = std::remove_cvref_t<decltype(value)>;
              if constexpr (std::is_same_v<Source,
                                           CompositionDirectModelAnchor>) {
                value.root_pose_in_cell = pose;
              } else if constexpr (std::is_same_v<
                                       Source,
                                       CompositionDirectComponentAnchor>) {
                value.root_pose_in_cell = pose;
              } else if constexpr (std::is_same_v<Source,
                                                  CompositionModelMount>) {
                value.child_frame_pose_in_parent_frame = pose;
              } else {
                value.child_interface_pose_in_parent_interface = pose;
              }
              return value;
            },
            source),
        require_map_text(row, "compiled_joint_name"),
        require_map_text(row, "compiled_parent_link_name"),
        require_map_text(row, "compiled_child_link_name"), pose});
  }

  const auto& frame_paths = require_map_array(map, "frame_paths");
  result.frame_paths.reserve(frame_paths.size());
  for (const auto& row : frame_paths) {
    std::vector<CompositionFramePathSegment> segments;
    const auto& segment_rows = require_map_array(row, "segments");
    segments.reserve(segment_rows.size());
    for (const auto& segment : segment_rows) {
      segments.push_back(decode_frame_path_segment(segment));
    }
    result.frame_paths.push_back(CompositionFramePath{
        decode_resolved_frame(require_map_member(row, "terminal_frame")),
        std::move(segments)});
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

[[nodiscard]] core::ResolvedFrameDTO runtime_resolved_frame(
    const CompositionResolvedFrame& frame) {
  return std::visit(
      [](const auto& value) -> core::ResolvedFrameDTO {
        using Frame = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Frame, CompositionCellFrame>) {
          return core::ResolvedCellFrameDTO{
              value.cell_id, value.compiled_frame_name};
        } else if constexpr (std::is_same_v<
                                 Frame, CompositionComponentModelFrame>) {
          return core::ResolvedComponentModelFrameDTO{
              value.controllable_component_id, value.model_instance_id,
              value.frame_role_id, value.compiled_frame_name,
              value.model_revision};
        } else {
          return core::ResolvedPassiveModelFrameDTO{
              value.model_instance_id, value.frame_role_id,
              value.compiled_frame_name, value.model_revision};
        }
      },
      frame);
}

[[nodiscard]] core::KinematicJointCoordinateKind runtime_coordinate_kind(
    UrdfCoordinateKind kind) {
  switch (kind) {
    case UrdfCoordinateKind::kNone:
      return core::KinematicJointCoordinateKind::kFixed;
    case UrdfCoordinateKind::kAngular:
      return core::KinematicJointCoordinateKind::kAngular;
    case UrdfCoordinateKind::kLinear:
      return core::KinematicJointCoordinateKind::kLinear;
    case UrdfCoordinateKind::kPlanar:
      return core::KinematicJointCoordinateKind::kPlanar;
    case UrdfCoordinateKind::kFloating:
      return core::KinematicJointCoordinateKind::kFloating;
  }
  detail::fail_retained_bundle(
      "retained model map has an unknown runtime coordinate kind");
}

[[nodiscard]] std::optional<core::CoordinateKind>
runtime_scalar_coordinate_kind(UrdfCoordinateKind kind) {
  switch (kind) {
    case UrdfCoordinateKind::kAngular:
      return core::CoordinateKind::kAngular;
    case UrdfCoordinateKind::kLinear:
      return core::CoordinateKind::kLinear;
    case UrdfCoordinateKind::kNone:
    case UrdfCoordinateKind::kPlanar:
    case UrdfCoordinateKind::kFloating:
      return std::nullopt;
  }
  detail::fail_retained_bundle(
      "retained model map has an unknown scalar coordinate kind");
}

struct RuntimePathBuildIndex final {
  std::map<std::string, const CompositionModelOccurrence*, std::less<>>
      model_occurrences;
  std::map<std::string, const core::ControlBindingDTO*, std::less<>>
      control_bindings;
  std::map<std::string, core::ControlDomainRevision, std::less<>>
      control_domains;
};

[[nodiscard]] RuntimePathBuildIndex make_runtime_path_build_index(
    const DecodedRetainedModelMap& decoded,
    const core::CellGraphDTO& graph) {
  RuntimePathBuildIndex result;
  for (const auto& occurrence : decoded.model_occurrences) {
    if (!result.model_occurrences
             .emplace(occurrence.model_instance_id.value(), &occurrence)
             .second) {
      detail::fail_retained_bundle(
          "retained model map repeats a model occurrence while building "
          "runtime paths");
    }
  }
  for (const auto& domain : graph.control_domains) {
    if (!result.control_domains
             .emplace(domain.control_domain_id.value(),
                      domain.control_domain_revision)
             .second) {
      detail::fail_retained_bundle(
          "retained graph repeats a control domain while building runtime "
          "paths");
    }
  }
  for (const auto& binding : graph.control_bindings) {
    if (!result.control_bindings
             .emplace(binding.control_binding_id.value(), &binding)
             .second) {
      detail::fail_retained_bundle(
          "retained graph repeats a control binding while building runtime "
          "paths");
    }
    if (!result.control_domains.contains(
            binding.control_domain_id.value())) {
      detail::fail_retained_bundle(
          "retained binding references an absent control domain while "
          "building runtime paths");
    }
  }
  return result;
}

[[nodiscard]] const CompositionModelOccurrence& require_runtime_occurrence(
    const RuntimePathBuildIndex& index,
    const core::ModelInstanceId& model_instance_id) {
  const auto found = index.model_occurrences.find(model_instance_id.value());
  if (found == index.model_occurrences.end()) {
    detail::fail_retained_bundle(
        "retained runtime path references an absent model occurrence");
  }
  return *found->second;
}

using RuntimeDependencyKey =
    std::tuple<std::string, std::string, std::string, std::string,
               std::string>;

[[nodiscard]] RuntimeDependencyKey runtime_dependency_key(
    const core::KinematicResourceCoordinateDependencyDTO& dependency) {
  return {dependency.control_domain_id.value(),
          dependency.control_domain_revision.value(),
          dependency.control_binding_id.value(),
          dependency.control_binding_revision.value(),
          dependency.resource_id.value()};
}

[[nodiscard]] std::vector<core::KinematicResourceCoordinateDependencyDTO>
build_runtime_coordinate_dependencies(
    const CompositionModelCoordinate& coordinate,
    const DecodedRetainedModelMap& decoded,
    const RuntimePathBuildIndex& index) {
  std::map<RuntimeDependencyKey,
           core::KinematicResourceCoordinateDependencyDTO>
      dependencies;
  for (const auto& binding_map : decoded.control_bindings) {
    const auto graph_binding = index.control_bindings.find(
        binding_map.control_binding_id.value());
    if (graph_binding == index.control_bindings.end()) {
      detail::fail_retained_bundle(
          "retained coordinate dependency references an absent control "
          "binding");
    }
    const auto domain_revision = index.control_domains.find(
        graph_binding->second->control_domain_id.value());
    if (domain_revision == index.control_domains.end()) {
      detail::fail_retained_bundle(
          "retained coordinate dependency references an absent control "
          "domain");
    }
    for (const auto& resource : binding_map.resources) {
      for (const auto& projection : resource.coordinates) {
        if (projection.model_instance_id != coordinate.model_instance_id ||
            projection.source_joint_name != coordinate.source_joint_name ||
            projection.compiled_joint_name !=
                coordinate.compiled_joint_name) {
          continue;
        }
        if (projection.coordinate_kind != coordinate.coordinate_kind ||
            projection.unit != coordinate.unit) {
          detail::fail_retained_bundle(
              "retained coordinate dependency disagrees with its exact "
              "model coordinate kind or unit");
        }
        const auto scalar_kind =
            runtime_scalar_coordinate_kind(projection.coordinate_kind);
        if (!scalar_kind.has_value()) {
          detail::fail_retained_bundle(
              "retained coordinate dependency is not an angular or linear "
              "resource");
        }
        core::KinematicResourceCoordinateDependencyDTO dependency{
            graph_binding->second->control_domain_id,
            domain_revision->second,
            graph_binding->second->control_binding_id,
            graph_binding->second->control_binding_revision,
            resource.resource_id,
            *scalar_kind,
            projection.multiplier,
            projection.offset,
            projection.derived_mimic};
        if (!dependencies
                 .emplace(runtime_dependency_key(dependency), dependency)
                 .second) {
          detail::fail_retained_bundle(
              "retained runtime path repeats one exact resource-coordinate "
              "dependency");
        }
      }
    }
  }

  std::vector<core::KinematicResourceCoordinateDependencyDTO> result;
  result.reserve(dependencies.size());
  for (auto& [unused, dependency] : dependencies) {
    (void)unused;
    result.push_back(std::move(dependency));
  }
  return result;
}

[[nodiscard]] core::KinematicFramePathSegmentDTO runtime_fixed_segment(
    const CompositionFixedConnection& connection,
    const RuntimePathBuildIndex& index) {
  return std::visit(
      [&connection, &index](const auto& source)
          -> core::KinematicFramePathSegmentDTO {
        using Source = std::remove_cvref_t<decltype(source)>;
        if constexpr (std::is_same_v<Source,
                                     CompositionDirectModelAnchor>) {
          const auto& occurrence = require_runtime_occurrence(
              index, source.model_instance_id);
          return core::KinematicDirectModelAnchorPathSegmentDTO{
              source.model_instance_id, occurrence.model_revision,
              connection.compiled_joint_name,
              connection.compiled_parent_link_name,
              connection.compiled_child_link_name,
              connection.child_pose_in_parent};
        } else if constexpr (std::is_same_v<
                                 Source, CompositionDirectComponentAnchor>) {
          const auto& occurrence = require_runtime_occurrence(
              index, source.root_model_instance_id);
          return core::KinematicDirectComponentAnchorPathSegmentDTO{
              source.controllable_component_id,
              source.root_model_instance_id, occurrence.model_revision,
              connection.compiled_joint_name,
              connection.compiled_parent_link_name,
              connection.compiled_child_link_name,
              connection.child_pose_in_parent};
        } else if constexpr (std::is_same_v<Source,
                                            CompositionModelMount>) {
          const auto& parent = require_runtime_occurrence(
              index, source.parent_model_instance_id);
          const auto& child = require_runtime_occurrence(
              index, source.child_model_instance_id);
          return core::KinematicModelMountPathSegmentDTO{
              source.controllable_component_id,
              source.parent_model_instance_id, parent.model_revision,
              source.child_model_instance_id, child.model_revision,
              connection.compiled_joint_name,
              connection.compiled_parent_link_name,
              connection.compiled_child_link_name,
              connection.child_pose_in_parent};
        } else {
          return core::KinematicComponentAttachmentPathSegmentDTO{
              source.component_attachment_id, source.parent_component_id,
              source.child_component_id, connection.compiled_joint_name,
              connection.compiled_parent_link_name,
              connection.compiled_child_link_name,
              connection.child_pose_in_parent};
        }
      },
      connection.source);
}

[[nodiscard]] std::vector<core::KinematicFramePathDTO>
build_runtime_kinematic_paths(const DecodedRetainedModelMap& decoded,
                              const core::CellGraphDTO& graph) {
  const RuntimePathBuildIndex index =
      make_runtime_path_build_index(decoded, graph);
  std::vector<core::KinematicFramePathDTO> result;
  result.reserve(decoded.frame_paths.size());
  for (const auto& path : decoded.frame_paths) {
    std::vector<core::KinematicFramePathSegmentDTO> segments;
    segments.reserve(path.segments.size());
    for (const auto& segment : path.segments) {
      std::visit(
          [&decoded, &index, &segments](const auto& value) {
            using Segment = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Segment,
                                         CompositionFixedPathSegment>) {
              if (value.fixed_connection_index >=
                  decoded.fixed_connections.size()) {
                detail::fail_retained_bundle(
                    "retained runtime path has an out-of-range fixed "
                    "connection");
              }
              segments.push_back(runtime_fixed_segment(
                  decoded.fixed_connections[value.fixed_connection_index],
                  index));
            } else {
              if (value.composition_coordinate_index >=
                  decoded.coordinates.size()) {
                detail::fail_retained_bundle(
                    "retained runtime path has an out-of-range model "
                    "coordinate");
              }
              const auto& coordinate = decoded.coordinates[
                  value.composition_coordinate_index];
              if (coordinate.model_instance_id != value.model_instance_id ||
                  coordinate.source_joint_name != value.source_joint_name ||
                  coordinate.compiled_joint_name !=
                      value.compiled_joint_name) {
                detail::fail_retained_bundle(
                    "retained runtime path model-joint segment differs from "
                    "its exact coordinate");
              }
              const auto& occurrence = require_runtime_occurrence(
                  index, value.model_instance_id);
              segments.emplace_back(core::KinematicModelJointPathSegmentDTO{
                  coordinate.owner_component_id,
                  value.model_instance_id,
                  occurrence.model_revision,
                  value.composition_coordinate_index,
                  value.source_joint_name,
                  value.compiled_joint_name,
                  value.compiled_parent_link_name,
                  value.compiled_child_link_name,
                  runtime_coordinate_kind(coordinate.coordinate_kind),
                  build_runtime_coordinate_dependencies(
                      coordinate, decoded, index)});
            }
          },
          segment);
    }
    result.push_back(core::KinematicFramePathDTO{
        runtime_resolved_frame(path.terminal_frame), std::move(segments)});
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

struct RetainedControlCatalogClosure final {
  std::vector<QualifiedControlInterfaceContract> control_interfaces;
  std::vector<QualifiedControllerTopology> controller_topologies;
  std::vector<QualifiedProviderManifest> provider_manifests;
  std::vector<QualifiedStopEvidencePolicy> stop_policies;
};

[[nodiscard]] RetainedControlCatalogClosure
load_retained_control_catalog_closure(
    const std::filesystem::path& catalog_data,
    std::span<const core::CatalogRecordRefDTO> records) {
  using ControlInput = std::pair<
      core::ControlInterfaceContractCatalogRecordRefDTO, std::string>;
  using TopologyInput =
      std::pair<core::ControllerTopologyCatalogRecordRefDTO, std::string>;
  using ManifestInput =
      std::pair<core::ProviderManifestCatalogRecordRefDTO, std::string>;
  using PolicyInput =
      std::pair<core::StopEvidencePolicyCatalogRecordRefDTO, std::string>;
  std::vector<ControlInput> control_inputs;
  std::vector<TopologyInput> topology_inputs;
  std::vector<ManifestInput> manifest_inputs;
  std::vector<PolicyInput> policy_inputs;

  for (const auto& record : records) {
    std::visit(
        [&](const auto& typed_record) {
          std::string bytes = read_verified_catalog_reference(
              catalog_data, typed_record.content_addressed_store_key,
              typed_record.content_digest, std::nullopt,
              "retained catalog-record reference");
          using Record = std::decay_t<decltype(typed_record)>;
          if constexpr (std::is_same_v<
                            Record,
                            core::ControlInterfaceContractCatalogRecordRefDTO>) {
            control_inputs.emplace_back(typed_record, std::move(bytes));
          } else if constexpr (
              std::is_same_v<Record,
                             core::ControllerTopologyCatalogRecordRefDTO>) {
            topology_inputs.emplace_back(typed_record, std::move(bytes));
          } else if constexpr (
              std::is_same_v<Record,
                             core::ProviderManifestCatalogRecordRefDTO>) {
            manifest_inputs.emplace_back(typed_record, std::move(bytes));
          } else if constexpr (
              std::is_same_v<Record,
                             core::StopEvidencePolicyCatalogRecordRefDTO>) {
            policy_inputs.emplace_back(typed_record, std::move(bytes));
          }
        },
        record);
  }

  RetainedControlCatalogClosure result;
  result.control_interfaces.reserve(control_inputs.size());
  for (const auto& [reference, bytes] : control_inputs) {
    result.control_interfaces.push_back(
        resolve_and_qualify_control_interface_contract(
            reference, reference.content_addressed_store_key, bytes));
  }
  result.controller_topologies.reserve(topology_inputs.size());
  for (const auto& [reference, bytes] : topology_inputs) {
    result.controller_topologies.push_back(resolve_and_qualify_controller_topology(
        reference, reference.content_addressed_store_key, bytes,
        result.control_interfaces));
  }
  result.provider_manifests.reserve(manifest_inputs.size());
  for (const auto& [reference, bytes] : manifest_inputs) {
    result.provider_manifests.push_back(resolve_and_qualify_provider_manifest(
        reference, reference.content_addressed_store_key, bytes));
  }
  result.stop_policies.reserve(policy_inputs.size());
  for (const auto& [reference, bytes] : policy_inputs) {
    result.stop_policies.push_back(resolve_and_qualify_stop_evidence_policy(
        reference, reference.content_addressed_store_key, bytes));
  }
  return result;
}

[[nodiscard]] RetainedControlCatalogClosure select_control_catalogs_for_domain(
    const RetainedControlCatalogClosure& available,
    const core::CellGraphDTO& graph,
    const core::ControlDomainId& control_domain_id) {
  RetainedControlCatalogClosure result;
  const auto domain = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [&](const auto& candidate) {
        return candidate.control_domain_id == control_domain_id;
      });
  if (domain == graph.control_domains.end()) {
    detail::fail_retained_bundle(
        "cannot select catalogs for an unknown control domain");
  }
  for (const auto& topology : available.controller_topologies) {
    if (topology.value().controller_topology_id ==
            domain->controller_topology_id &&
        topology.value().topology_revision == domain->topology_revision) {
      result.controller_topologies.push_back(topology);
    }
  }
  std::set<std::pair<std::string, std::string>> policy_ids;
  std::set<std::pair<std::string, std::string>> manifest_ids;
  for (const auto& binding : graph.control_bindings) {
    if (binding.control_domain_id != control_domain_id) {
      continue;
    }
    policy_ids.emplace(binding.stop_evidence_policy_id.value(),
                       binding.stop_evidence_policy_revision.value());
    manifest_ids.emplace(binding.provider_manifest_id.value(),
                         binding.provider_manifest_revision.value());
  }
  for (const auto& policy : available.stop_policies) {
    if (policy_ids.contains(
            {policy.value().stop_evidence_policy_id.value(),
             policy.value().stop_evidence_policy_revision.value()})) {
      result.stop_policies.push_back(policy);
    }
  }
  for (const auto& manifest : available.provider_manifests) {
    if (manifest_ids.contains(
            {manifest.value().provider_manifest_id.value(),
             manifest.value().provider_manifest_revision.value()})) {
      result.provider_manifests.push_back(manifest);
    }
  }
  return result;
}

void require_verified_product_artifacts(
    const std::filesystem::path &raw_data,
    const core::RetainedProductDTO &product,
    const core::RetainedBundleDTO &bundle,
    const core::ControlDomainId *control_domain_subject = nullptr,
    const core::RuntimeUnitId* runtime_unit_subject = nullptr,
    std::span<const QualifiedStopEvidencePolicy> stop_policies = {},
    std::span<const QualifiedControllerTopology> controller_topologies = {},
    std::span<const QualifiedProviderManifest> provider_manifests = {}) {
  std::optional<std::string> runtime_manifest;
  std::optional<std::string> runtime_launchable;
  std::optional<std::string> runtime_implementation_build_identity;
  std::optional<std::string> control_domain_control;
  std::optional<std::string> stop_implementation_evidence;
  std::optional<std::string> stop_substitution_evidence;
  std::optional<std::string> stop_commissioning_evidence;
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
      const std::string_view role = artifact.artifact_role_id.value();
      if (role == detail::kControlDomainControlArtifactRole) {
        if (artifact.media_type != detail::kControlDomainControlMediaType ||
            control_domain_control.has_value()) {
          detail::fail_retained_bundle(
              "control-domain product has an incoherent primary artifact");
        }
        control_domain_control = bytes;
      } else if (role ==
                 core::kStopImplementationQualificationArtifactRole) {
        if (artifact.media_type !=
                core::kStopImplementationQualificationMediaType ||
            stop_implementation_evidence.has_value()) {
          detail::fail_retained_bundle(
              "control-domain product has incoherent implementation evidence");
        }
        stop_implementation_evidence = bytes;
      } else if (role ==
                 core::kControlledStopSubstitutionQualificationArtifactRole) {
        if (artifact.media_type !=
                core::kControlledStopSubstitutionQualificationMediaType ||
            stop_substitution_evidence.has_value()) {
          detail::fail_retained_bundle(
              "control-domain product has incoherent substitution evidence");
        }
        stop_substitution_evidence = bytes;
      } else if (role == core::kStopInterfaceCommissioningArtifactRole) {
        if (artifact.media_type != core::kStopInterfaceCommissioningMediaType ||
            stop_commissioning_evidence.has_value()) {
          detail::fail_retained_bundle(
              "control-domain product has incoherent commissioning evidence");
        }
        stop_commissioning_evidence = bytes;
      } else {
        detail::fail_retained_bundle(
            "control-domain product has an unexpected artifact role");
      }
    } else if (product.product_kind ==
               core::RetainedProductKind::kRuntimeUnitConfiguration) {
      if (runtime_unit_subject == nullptr) {
        detail::fail_retained_bundle("runtime-unit product is missing its envelope subject");
      }
      const bool native_execution =
          product.product_contract_id.value() ==
              detail::kNativeExecutionRuntimeProductContractId &&
          product.product_contract_revision ==
              detail::native_execution_runtime_product_contract_revision();
      const bool ros2_control_execution =
          product.product_contract_id.value() ==
              detail::kRos2ControlExecutionRuntimeProductContractId &&
          product.product_contract_revision ==
              detail::ros2_control_execution_runtime_product_contract_revision();
      const std::string_view manifest_role =
          native_execution
              ? detail::kNativeExecutionRuntimeManifestArtifactRole
          : ros2_control_execution
              ? detail::kRos2ControlExecutionRuntimeManifestArtifactRole
              : detail::kReferenceRuntimeManifestArtifactRole;
      const std::string_view manifest_media =
          native_execution
              ? detail::kNativeExecutionRuntimeManifestMediaType
          : ros2_control_execution
              ? detail::kRos2ControlExecutionRuntimeManifestMediaType
              : detail::kReferenceRuntimeManifestMediaType;
      const std::string_view launchable_role =
          native_execution
              ? detail::kNativeExecutionRuntimeLaunchDescriptorArtifactRole
          : ros2_control_execution
              ? detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole
              : detail::kReferenceRuntimeLaunchableArtifactRole;
      const std::string_view launchable_media =
          native_execution
              ? detail::kNativeExecutionRuntimeLaunchDescriptorMediaType
          : ros2_control_execution
              ? detail::kRos2ControlExecutionRuntimeLaunchDescriptorMediaType
              : detail::kReferenceRuntimeLaunchableMediaType;
      if (artifact.artifact_role_id.value() == manifest_role) {
        if (artifact.media_type != manifest_media ||
            runtime_manifest.has_value()) {
          detail::fail_retained_bundle(
              "runtime-unit product has an incoherent manifest artifact");
        }
        runtime_manifest = bytes;
      } else if (artifact.artifact_role_id.value() == launchable_role) {
        if (artifact.media_type != launchable_media ||
            runtime_launchable.has_value()) {
          detail::fail_retained_bundle(
              "runtime-unit product has an incoherent launchable artifact");
        }
        runtime_launchable = bytes;
      } else if ((native_execution || ros2_control_execution) &&
                 artifact.artifact_role_id.value() ==
                     (native_execution
                          ? detail::kNativeExecutionRuntimeBuildIdentityArtifactRole
                          : detail::kRos2ControlExecutionRuntimeBuildIdentityArtifactRole)) {
        const std::string_view expected_media =
            native_execution
                ? detail::kNativeExecutionRuntimeBuildIdentityMediaType
                : detail::kRos2ControlExecutionRuntimeBuildIdentityMediaType;
        if (artifact.media_type != expected_media ||
            runtime_implementation_build_identity.has_value()) {
          detail::fail_retained_bundle(
              "execution runtime product has an incoherent implementation "
              "build identity");
        }
        runtime_implementation_build_identity = bytes;
      } else {
        detail::fail_retained_bundle("runtime-unit product has an unexpected artifact role");
      }
    }
  }
  if (product.product_kind ==
      core::RetainedProductKind::kControlDomainControl) {
    if (control_domain_subject == nullptr ||
        !control_domain_control.has_value()) {
      detail::fail_retained_bundle(
          "control-domain product is missing its envelope subject or primary "
          "artifact");
    }
    std::vector<std::pair<core::ProductId, std::string>>
        execution_build_identity_storage;
    execution_build_identity_storage.reserve(
        bundle.compiled_products.runtime_unit_products.size());
    for (const auto& runtime_group :
         bundle.compiled_products.runtime_unit_products) {
      if (runtime_group.products.size() != 1U) {
        detail::fail_retained_bundle(
            "control readback found an incoherent runtime product group");
      }
      const auto& runtime_product = runtime_group.products.front();
      const bool native_execution =
          runtime_product.product_contract_id.value() ==
              detail::kNativeExecutionRuntimeProductContractId &&
          runtime_product.product_contract_revision ==
              detail::native_execution_runtime_product_contract_revision();
      const bool ros2_control_execution =
          runtime_product.product_contract_id.value() ==
              detail::kRos2ControlExecutionRuntimeProductContractId &&
          runtime_product.product_contract_revision ==
              detail::ros2_control_execution_runtime_product_contract_revision();
      if (!native_execution && !ros2_control_execution) {
        continue;
      }
      const std::string_view identity_role =
          native_execution
              ? detail::kNativeExecutionRuntimeBuildIdentityArtifactRole
              : detail::kRos2ControlExecutionRuntimeBuildIdentityArtifactRole;
      const std::string_view identity_media =
          native_execution
              ? detail::kNativeExecutionRuntimeBuildIdentityMediaType
              : detail::kRos2ControlExecutionRuntimeBuildIdentityMediaType;
      const auto identity_ref = std::find_if(
          runtime_product.artifact_refs.begin(),
          runtime_product.artifact_refs.end(), [&](const auto& artifact) {
            return artifact.artifact_role_id.value() == identity_role;
          });
      if (identity_ref == runtime_product.artifact_refs.end()) {
        continue;
      }
      if (identity_ref->media_type != identity_media) {
        detail::fail_retained_bundle(
            "execution runtime build identity has an incoherent media type");
      }
      execution_build_identity_storage.emplace_back(
          runtime_product.product_id,
          read_verified_raw_reference(
              raw_data, identity_ref->content_addressed_store_key,
              identity_ref->content_digest, identity_ref->byte_size,
              "execution runtime implementation build identity"));
    }
    std::vector<detail::ExecutionRuntimeBuildIdentityArtifact>
        execution_build_identities;
    execution_build_identities.reserve(
        execution_build_identity_storage.size());
    for (const auto& [product_id, bytes] :
         execution_build_identity_storage) {
      execution_build_identities.push_back({product_id, bytes});
    }
    detail::require_control_domain_control_matches_retained_bundle(
        detail::ControlDomainControlArtifactClosure{
            *control_domain_control,
            stop_implementation_evidence.has_value()
                ? std::optional<std::string_view>(*stop_implementation_evidence)
                : std::nullopt,
            stop_substitution_evidence.has_value()
                ? std::optional<std::string_view>(*stop_substitution_evidence)
                : std::nullopt,
            stop_commissioning_evidence.has_value()
                ? std::optional<std::string_view>(*stop_commissioning_evidence)
                : std::nullopt,
            execution_build_identities},
        bundle, *control_domain_subject, stop_policies, controller_topologies,
        provider_manifests);
  }
  if (product.product_kind ==
      core::RetainedProductKind::kRuntimeUnitConfiguration) {
    if (!runtime_manifest.has_value() || !runtime_launchable.has_value()) {
      detail::fail_retained_bundle(
          "runtime-unit product omits its manifest or launchable artifact");
    }
    if (product.product_contract_id.value() ==
            detail::kNativeExecutionRuntimeProductContractId &&
        product.product_contract_revision ==
            detail::native_execution_runtime_product_contract_revision()) {
      detail::require_native_execution_runtime_product_matches_retained_bundle(
          *runtime_manifest, *runtime_launchable,
          runtime_implementation_build_identity.has_value()
              ? std::optional<std::string_view>(
                    *runtime_implementation_build_identity)
              : std::nullopt,
          bundle,
          *runtime_unit_subject);
    } else if (product.product_contract_id.value() ==
                   detail::kRos2ControlExecutionRuntimeProductContractId &&
               product.product_contract_revision ==
                   detail::ros2_control_execution_runtime_product_contract_revision()) {
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          *runtime_manifest, *runtime_launchable,
          runtime_implementation_build_identity.has_value()
              ? std::optional<std::string_view>(
                    *runtime_implementation_build_identity)
              : std::nullopt,
          bundle, *runtime_unit_subject);
    } else {
      detail::require_reference_runtime_product_matches_retained_bundle(
          *runtime_manifest, *runtime_launchable, bundle,
          *runtime_unit_subject);
    }
  }
}

void require_verified_bundle_references(
    const std::filesystem::path& trusted_root,
    const std::filesystem::path& raw_data, const RetainedBundle& bundle) {
  const auto& value = bundle.value();
  RetainedControlCatalogClosure control_catalogs;
  if (!value.catalogs.records.empty()) {
    const std::filesystem::path catalog_data =
        require_read_only_catalog_data(trusted_root);
    control_catalogs = load_retained_control_catalog_closure(
        catalog_data, value.catalogs.records);
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
    const RetainedControlCatalogClosure domain_catalogs =
        select_control_catalogs_for_domain(
            control_catalogs, value.cell_graph, group.control_domain_id);
    for (const auto &product : group.products) {
      require_verified_product_artifacts(raw_data, product, value,
                                         &group.control_domain_id, nullptr,
                                         domain_catalogs.stop_policies,
                                         domain_catalogs.controller_topologies,
                                         domain_catalogs.provider_manifests);
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

const CompositionControlBindingCoordinateMap*
VerifiedRetainedModelMap::find_binding(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& command_target_id,
    const core::ControlBindingId& control_binding_id) const noexcept {
  const CompositionControlBindingCoordinateMap* match = nullptr;
  for (const auto& candidate : control_bindings_) {
    if (candidate.controllable_component_id != component_id ||
        candidate.command_target_id != command_target_id ||
        candidate.control_binding_id != control_binding_id) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &candidate;
  }
  return match;
}

const core::KinematicFramePathDTO*
VerifiedRetainedModelMap::find_kinematic_frame_path(
    const core::ResolvedFrameDTO& terminal_frame) const noexcept {
  const core::KinematicFramePathDTO* match = nullptr;
  for (const auto& candidate : kinematic_frame_paths_) {
    if (candidate.terminal_frame != terminal_frame) {
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

FilesystemVerifiedCatalogArtifactSource::
    FilesystemVerifiedCatalogArtifactSource(
        std::filesystem::path trusted_package_root,
        const std::uint64_t maximum_single_payload_bytes)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)),
      maximum_single_payload_bytes_(maximum_single_payload_bytes) {
  if (maximum_single_payload_bytes_ == 0U ||
      maximum_single_payload_bytes_ >
          std::numeric_limits<std::uint64_t>::max() -
              detail::kMaximumCatalogArtifactEnvelopeOverhead) {
    throw std::invalid_argument(
        "read-only catalog-artifact byte bound is invalid");
  }
}

std::optional<std::string>
FilesystemVerifiedCatalogArtifactSource::get_verified(
    const core::ContentAddressedStoreKey& exact_key,
    const core::ContentDigest& expected_digest,
    const std::optional<std::uint64_t> expected_size) const {
  const auto current_root = require_unchanged_trusted_root(
      trusted_package_root_, "read-only catalog-artifact");
  const auto catalog_data = find_read_only_data_directory(
      current_root, detail::kCatalogArtifactNamespaceDirectory,
      detail::kCatalogArtifactDataDirectory);
  if (!catalog_data.has_value()) {
    return std::nullopt;
  }
  return read_verified_catalog_entry(
      *catalog_data, exact_key,
      expected_digest, expected_size, maximum_single_payload_bytes_);
}

FilesystemVerifiedProgramSource::FilesystemVerifiedProgramSource(
    std::filesystem::path trusted_package_root,
    const std::uint64_t maximum_single_program_bytes)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)),
      maximum_single_program_bytes_(maximum_single_program_bytes) {
  if (maximum_single_program_bytes_ == 0U) {
    throw std::invalid_argument("read-only Program byte bound is invalid");
  }
}

std::shared_ptr<const ProgramBindingContract>
FilesystemVerifiedProgramSource::get_verified(
    const core::ProgramId& program_id,
    const core::ProgramRevision& program_revision,
    const RetainedBundle& retained_bundle) const {
  const auto hex = detail::retained_digest_hex(program_revision.value());
  if (!hex.has_value()) {
    return nullptr;
  }
  const auto current_root = require_unchanged_trusted_root(
      trusted_package_root_, "read-only Program");
  const auto program_data = find_read_only_data_directory(
      current_root, "programs", "sha256");
  if (!program_data.has_value()) {
    return nullptr;
  }
  const auto bytes = posix::read_exact_regular_file(
      *program_data / (std::string(*hex) + ".json"),
      maximum_single_program_bytes_, true, kPosixErrors, kReadMessages);
  if (!bytes.has_value()) {
    return nullptr;
  }
  const auto program =
      verify_program_binding_contract(*bytes, retained_bundle);
  if (program->value().program_id != program_id ||
      program->value().program_revision != program_revision) {
    throw ProgramContractError(
        "read-only Program key/content or Program identity mismatch");
  }
  return program;
}

FilesystemVerifiedProgramBindingSource::
    FilesystemVerifiedProgramBindingSource(
        std::filesystem::path trusted_package_root,
        const std::uint64_t maximum_single_binding_bytes)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)),
      maximum_single_binding_bytes_(maximum_single_binding_bytes) {
  if (maximum_single_binding_bytes_ == 0U) {
    throw std::invalid_argument(
        "read-only Program-Binding byte bound is invalid");
  }
}

std::shared_ptr<const ProgramBinding>
FilesystemVerifiedProgramBindingSource::get_verified(
    const core::ProgramBindingId& program_binding_id,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map) const {
  const auto hex =
      detail::retained_digest_hex(program_binding_id.value());
  if (!hex.has_value()) {
    return nullptr;
  }
  const auto current_root = require_unchanged_trusted_root(
      trusted_package_root_, "read-only Program-Binding");
  const auto binding_data = find_read_only_data_directory(
      current_root, "program-bindings", "sha256");
  if (!binding_data.has_value()) {
    return nullptr;
  }
  const auto bytes = posix::read_exact_regular_file(
      *binding_data / (std::string(*hex) + ".json"),
      maximum_single_binding_bytes_, true, kPosixErrors, kReadMessages);
  if (!bytes.has_value()) {
    return nullptr;
  }
  const auto binding = reload_verified_program_binding(
      *bytes, retained_bundle, retained_model_map);
  if (binding->program_binding_id() != program_binding_id) {
    throw ProgramBindingError(
        "read-only Program-Binding key/content mismatch");
  }
  return binding;
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

FilesystemVerifiedRetainedProviderManifestSource::
    FilesystemVerifiedRetainedProviderManifestSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
  (void)require_read_only_catalog_data(trusted_package_root_);
}

std::optional<QualifiedProviderManifest>
FilesystemVerifiedRetainedProviderManifestSource::get_verified(
    const RetainedProviderManifestQuery &query) const {
  const std::filesystem::path current_root = require_unchanged_trusted_root(
      trusted_package_root_, "read-only retained-provider-manifest");
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle =
      read_verified_bundle_entry(paths.bundle_data, query.retained_bundle_id);
  if (bundle == nullptr) {
    return std::nullopt;
  }
  const core::ProviderManifestCatalogRecordRefDTO *selected = nullptr;
  for (const auto &record : bundle->value().catalogs.records) {
    const auto *manifest =
        std::get_if<core::ProviderManifestCatalogRecordRefDTO>(&record);
    if (manifest == nullptr ||
        manifest->record_id != query.provider_manifest_id ||
        manifest->record_revision != query.provider_manifest_revision) {
      continue;
    }
    if (selected != nullptr) {
      detail::fail_retained_bundle(
          "retained provider manifest selection is ambiguous");
    }
    selected = manifest;
  }
  if (selected == nullptr) {
    return std::nullopt;
  }
  const auto catalog_data = require_read_only_catalog_data(current_root);
  const std::string bytes = read_verified_catalog_reference(
      catalog_data, selected->content_addressed_store_key,
      selected->content_digest, std::nullopt,
      "selected retained provider manifest");
  auto qualified = resolve_and_qualify_provider_manifest(
      *selected, selected->content_addressed_store_key, bytes);
  if (qualified.value().provider_manifest_id != query.provider_manifest_id ||
      qualified.value().provider_manifest_revision !=
          query.provider_manifest_revision) {
    detail::fail_retained_bundle(
        "selected retained provider manifest changed identity");
  }
  return qualified;
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
    const RetainedControlCatalogClosure control_catalogs =
        load_retained_control_catalog_closure(
            require_read_only_catalog_data(current_root),
            bundle->value().catalogs.records);
    const RetainedControlCatalogClosure domain_catalogs =
        select_control_catalogs_for_domain(
            control_catalogs, bundle->value().cell_graph, *subject);
    require_verified_product_artifacts(paths.raw_data, *product,
                                       bundle->value(), subject, nullptr,
                                       domain_catalogs.stop_policies,
                                       domain_catalogs.controller_topologies,
                                       domain_catalogs.provider_manifests);
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

std::optional<VerifiedRetainedRuntimeUnitProduct> verify_runtime_unit_product(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const IVerifiedRetainedProductArtifactSource &artifact_source) {
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
  const auto &product = group->products.front();
  const bool native_execution =
      product.product_contract_id.value() ==
          detail::kNativeExecutionRuntimeProductContractId &&
      product.product_contract_revision ==
          detail::native_execution_runtime_product_contract_revision();
  const bool ros2_control_execution =
      product.product_contract_id.value() ==
          detail::kRos2ControlExecutionRuntimeProductContractId &&
      product.product_contract_revision ==
          detail::ros2_control_execution_runtime_product_contract_revision();
  const bool reference =
      product.product_contract_id.value() ==
          detail::kReferenceRuntimeProductContractId &&
      product.product_contract_revision ==
          detail::reference_runtime_product_contract_revision();
  if (!native_execution && !ros2_control_execution && !reference) {
    detail::fail_retained_bundle(
        "runtime-unit product has no build-qualified verifier");
  }
  const std::string_view manifest_role =
      native_execution
          ? detail::kNativeExecutionRuntimeManifestArtifactRole
      : ros2_control_execution
          ? detail::kRos2ControlExecutionRuntimeManifestArtifactRole
          : detail::kReferenceRuntimeManifestArtifactRole;
  const std::string_view launchable_role =
      native_execution
          ? detail::kNativeExecutionRuntimeLaunchDescriptorArtifactRole
      : ros2_control_execution
          ? detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole
          : detail::kReferenceRuntimeLaunchableArtifactRole;
  const RetainedProductArtifactQuery manifest_query{
      bundle.retained_bundle_id(),
      product.product_id,
      core::RetainedProductKind::kRuntimeUnitConfiguration,
      core::ArtifactRoleId(std::string(manifest_role)),
  };
  const RetainedProductArtifactQuery launchable_query{
      bundle.retained_bundle_id(),
      product.product_id,
      core::RetainedProductKind::kRuntimeUnitConfiguration,
      core::ArtifactRoleId(std::string(launchable_role)),
  };
  const auto manifest = artifact_source.get_verified(manifest_query);
  const auto launchable = artifact_source.get_verified(launchable_query);
  std::optional<VerifiedRetainedProductArtifact> implementation_build_identity;
  if (native_execution || ros2_control_execution) {
    const std::string_view build_identity_role =
        native_execution
            ? detail::kNativeExecutionRuntimeBuildIdentityArtifactRole
            : detail::kRos2ControlExecutionRuntimeBuildIdentityArtifactRole;
    const auto identity_ref = std::find_if(
        product.artifact_refs.begin(), product.artifact_refs.end(),
        [&](const auto& artifact) {
          return artifact.artifact_role_id.value() == build_identity_role;
        });
    if (identity_ref != product.artifact_refs.end()) {
      const RetainedProductArtifactQuery identity_query{
          bundle.retained_bundle_id(),
          product.product_id,
          core::RetainedProductKind::kRuntimeUnitConfiguration,
          core::ArtifactRoleId(std::string(build_identity_role)),
      };
      implementation_build_identity =
          artifact_source.get_verified(identity_query);
      if (!implementation_build_identity.has_value() ||
          implementation_build_identity->query() != identity_query ||
          implementation_build_identity->product_contract_id() !=
              product.product_contract_id ||
          implementation_build_identity->product_contract_revision() !=
              product.product_contract_revision ||
          implementation_build_identity->artifact_ref() != *identity_ref) {
        detail::fail_retained_bundle(
            "runtime-unit artifact source did not return the exact execution "
            "build identity");
      }
    }
  }
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
  if (native_execution) {
    detail::require_native_execution_runtime_product_matches_retained_bundle(
        manifest->bytes(), launchable->bytes(),
        implementation_build_identity.has_value()
            ? std::optional<std::string_view>(
                  implementation_build_identity->bytes())
            : std::nullopt,
        bundle.value(),
        runtime_unit_id);
  } else if (ros2_control_execution) {
    detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
        manifest->bytes(), launchable->bytes(),
        implementation_build_identity.has_value()
            ? std::optional<std::string_view>(
                  implementation_build_identity->bytes())
            : std::nullopt,
        bundle.value(), runtime_unit_id);
  } else {
    detail::require_reference_runtime_product_matches_retained_bundle(
        manifest->bytes(), launchable->bytes(), bundle.value(),
        runtime_unit_id);
  }
  std::optional<VerifiedRuntimeImplementationBuildIdentity>
      verified_implementation_build_identity;
  if (implementation_build_identity.has_value()) {
    const auto identity = detail::parse_stop_implementation_build_identity(
        implementation_build_identity->bytes());
    verified_implementation_build_identity =
        VerifiedRuntimeImplementationBuildIdentity{
            identity.implementation_source_build_fingerprint,
            identity.installed_target_id,
            identity.installed_target_byte_size,
            identity.installed_target_content_digest};
  }
  return VerifiedRetainedRuntimeUnitProduct(
      bundle.retained_bundle_id(), runtime_unit_id, product.product_id,
      detail::retained_product_output_digest(product),
      manifest->artifact_ref().content_digest, product.product_contract_id,
      product.product_contract_revision, launchable->artifact_ref(),
      std::string(launchable->bytes()),
      native_execution
          ? std::string(detail::kNativeExecutionRuntimeOperatingSystem)
      : ros2_control_execution
          ? std::string(detail::kRos2ControlExecutionRuntimeOperatingSystem)
          : std::string(detail::kReferenceRuntimeOperatingSystem),
      native_execution
          ? std::string(
                detail::kNativeExecutionRuntimeLaunchEnvironmentContract)
      : ros2_control_execution
          ? std::string(
                detail::kRos2ControlExecutionRuntimeLaunchEnvironmentContract)
          : std::string(detail::kReferenceRuntimeShellContract),
      native_execution
          ? std::string(detail::kNativeExecutionRuntimeRoleId)
      : ros2_control_execution
          ? std::string(detail::kRos2ControlExecutionRuntimeRoleId)
          : std::string(detail::kReferenceRuntimeRoleId),
      native_execution
          ? std::string(detail::kNativeExecutionRuntimeStatusEndpointRoleId)
      : ros2_control_execution
          ? std::string(
                detail::kRos2ControlExecutionRuntimeStatusEndpointRoleId)
          : std::string(detail::kReferenceRuntimeStatusEndpointRoleId),
      native_execution
          ? std::string(detail::kNativeExecutionRuntimeStatusEndpointContractId)
      : ros2_control_execution
          ? std::string(
                detail::kRos2ControlExecutionRuntimeStatusEndpointContractId)
          : std::string(detail::kReferenceRuntimeStatusEndpointContractId),
      native_execution
          ? detail::native_execution_runtime_status_endpoint_revision()
      : ros2_control_execution
          ? detail::ros2_control_execution_runtime_status_endpoint_revision()
          : detail::reference_runtime_status_endpoint_revision(),
      (native_execution || ros2_control_execution) ? "adapter_private"
                                                   : "stdout_line",
      native_execution
          ? std::string(detail::kNativeExecutionRuntimeProcessAdapterContractId)
      : ros2_control_execution
          ? std::string(
                detail::kRos2ControlExecutionRuntimeProcessAdapterContractId)
          : std::string(detail::kReferenceRuntimeProcessAdapterContractId),
      native_execution
          ? detail::native_execution_runtime_process_adapter_revision()
      : ros2_control_execution
          ? detail::ros2_control_execution_runtime_process_adapter_revision()
          : detail::reference_runtime_process_adapter_revision(),
      native_execution
          ? std::string(detail::kNativeExecutionRuntimeRetrievalContractId)
      : ros2_control_execution
          ? std::string(
                detail::kRos2ControlExecutionRuntimeRetrievalContractId)
          : std::string(detail::kReferenceRuntimeRetrievalContractId),
      native_execution
          ? detail::native_execution_runtime_retrieval_contract_revision()
      : ros2_control_execution
          ? detail::ros2_control_execution_runtime_retrieval_contract_revision()
          : detail::reference_runtime_retrieval_contract_revision(),
      std::move(verified_implementation_build_identity));
}

std::optional<VerifiedRetainedRuntimeUnitProduct>
verify_reference_runtime_unit_product(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const IVerifiedRetainedProductArtifactSource &artifact_source) {
  auto product =
      verify_runtime_unit_product(bundle, runtime_unit_id, artifact_source);
  if (product.has_value() && product->product_contract_id().value() !=
                                 detail::kReferenceRuntimeProductContractId) {
    detail::fail_retained_bundle(
        "reference runtime verifier received a different runtime-product "
        "contract");
  }
  return product;
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
  std::vector<core::KinematicFramePathDTO> kinematic_frame_paths =
      build_runtime_kinematic_paths(decoded, bundle->value().cell_graph);

  return VerifiedRetainedModelMap(
      query, product->product_contract_id,
      product->product_contract_revision, std::move(decoded.cell_frame),
      std::move(decoded.model_occurrences),
      std::move(decoded.model_collision_bodies), std::move(decoded.symbols),
      std::move(decoded.references), std::move(decoded.coordinates),
      std::move(decoded.control_bindings),
      std::move(decoded.fixed_connections), std::move(decoded.frame_paths),
      std::move(kinematic_frame_paths), std::move(decoded.targets));
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

FilesystemVerifiedRetainedViewerModelSource::
    FilesystemVerifiedRetainedViewerModelSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
}

std::optional<VerifiedRetainedViewerModel>
FilesystemVerifiedRetainedViewerModelSource::get_verified(
    const RetainedViewerModelQuery &query) const {
  const std::filesystem::path current_root = require_unchanged_trusted_root(
      trusted_package_root_, "read-only retained viewer-model");
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle =
      read_verified_bundle_entry(paths.bundle_data, query.retained_bundle_id);
  if (bundle == nullptr) {
    return std::nullopt;
  }

  // This capability recognizes the sole cell-wide viewer placement. Equal
  // spelling in another product table can never select a model.
  const auto &product = bundle->value().compiled_products.viewer_model;
  if (!product.has_value() || product->product_id != query.product_id) {
    return std::nullopt;
  }
  if (product->product_kind != core::RetainedProductKind::kViewerModel) {
    detail::fail_retained_bundle(
        "selected viewer product has the wrong retained product kind");
  }

  // Full reference verification proves the code-owned viewer product has the
  // exact same compiled URDF and resource map as the authoritative planning
  // product. The narrower reader below then reconstructs only viewer inputs.
  require_verified_bundle_references(current_root, paths.raw_data, *bundle);
  const auto &resource_map_ref = require_exact_artifact_role(
      *product, detail::kPlanningResourceMapArtifactRole);
  const auto &urdf_ref = require_exact_artifact_role(*product, "urdf");
  const std::string resource_map_bytes = read_verified_raw_reference(
      paths.raw_data, resource_map_ref.content_addressed_store_key,
      resource_map_ref.content_digest, resource_map_ref.byte_size,
      "selected retained viewer resource map");
  std::string urdf = read_verified_raw_reference(
      paths.raw_data, urdf_ref.content_addressed_store_key,
      urdf_ref.content_digest, urdf_ref.byte_size,
      "selected retained viewer URDF");
  if (urdf.empty() || urdf.size() > kMaxModelSourceXmlEntrypointBytes) {
    detail::fail_retained_bundle(
        "retained viewer XML violates the fixed entrypoint bound");
  }

  const std::vector<detail::PlanningResourceMapRow> resource_map =
      detail::parse_and_verify_planning_resource_map(resource_map_bytes);
  std::vector<VerifiedRetainedPlanningResource> resources =
      read_verified_planning_resources(current_root, paths.raw_data,
                                       bundle->value(), resource_map);
  std::uint64_t result_bytes = static_cast<std::uint64_t>(urdf.size());
  for (const auto &resource : resources) {
    checked_accumulate(result_bytes, resource.byte_size(),
                       kMaxModelSourceClosureBytes,
                       "retained viewer result exceeds the global byte bound");
  }

  return VerifiedRetainedViewerModel(
      query, product->product_contract_id, product->product_contract_revision,
      urdf_ref, std::move(urdf), std::move(resources));
}

}  // namespace manyforge::deployment
