#include "manyforge_deployment/model_asset_codec.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw ModelAssetCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue& object, std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    fail("model-asset normalized value is missing " + std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view name) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("model-asset normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view name) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("model-asset normalized field is not an array: " +
         std::string(name));
  }
  return *array;
}

[[nodiscard]] std::uint64_t require_byte_size(const CanonicalValue& value,
                                              std::string_view name) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) ||
      std::floor(*number) != *number || *number < 1.0 ||
      *number > static_cast<double>(core::kMaxModelAssetByteSize)) {
    fail("model-asset normalized byte_size is outside the v1 safe range: " +
         std::string(name));
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] bool ascii_alphanumeric(unsigned char byte) noexcept {
  return (byte >= 'A' && byte <= 'Z') ||
         (byte >= 'a' && byte <= 'z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool safe_path_segment(std::string_view segment) noexcept {
  if (segment.empty() || segment.size() > 128U ||
      !ascii_alphanumeric(static_cast<unsigned char>(segment.front()))) {
    return false;
  }
  return std::all_of(
      segment.begin() + 1, segment.end(), [](unsigned char byte) {
        return ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
               byte == '-';
      });
}

void validate_logical_uri(std::string_view uri,
                          const core::ModelAssetId& model_asset_id) {
  if (uri.empty() || uri.size() > core::kMaxModelAssetLogicalUriBytes ||
      !std::all_of(uri.begin(), uri.end(),
                   [](unsigned char byte) { return byte <= 0x7fU; })) {
    fail("model source logical_uri must be non-empty bounded ASCII");
  }

  const std::string expected_prefix =
      "asset://" + model_asset_id.value() + "/";
  if (!uri.starts_with(expected_prefix)) {
    fail("model source logical_uri authority must equal model_asset_id");
  }
  const auto path = uri.substr(expected_prefix.size());
  if (path.empty()) {
    fail("model source logical_uri requires a safe relative path");
  }

  std::size_t segment_start = 0U;
  while (segment_start <= path.size()) {
    const auto slash = path.find('/', segment_start);
    const auto segment =
        path.substr(segment_start,
                    slash == std::string_view::npos
                        ? path.size() - segment_start
                        : slash - segment_start);
    if (!safe_path_segment(segment)) {
      fail("model source logical_uri contains an unsafe path segment");
    }
    if (slash == std::string_view::npos) {
      break;
    }
    segment_start = slash + 1U;
  }
}

[[nodiscard]] bool media_type_token_byte(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') ||
         (byte >= '0' && byte <= '9') || byte == '!' || byte == '#' ||
         byte == '$' || byte == '&' || byte == '^' || byte == '_' ||
         byte == '.' || byte == '+' || byte == '-';
}

void validate_media_type(std::string_view media_type) {
  if (media_type.size() < 3U ||
      media_type.size() > core::kMaxModelAssetMediaTypeBytes ||
      !std::all_of(media_type.begin(), media_type.end(),
                   [](unsigned char byte) { return byte <= 0x7fU; })) {
    fail("model source media_type is outside the v1 ASCII length bound");
  }
  const auto separator = media_type.find('/');
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U >= media_type.size() ||
      media_type.find('/', separator + 1U) != std::string_view::npos) {
    fail("model source media_type must contain exactly one non-edge slash");
  }
  const auto type = media_type.substr(0U, separator);
  const auto subtype = media_type.substr(separator + 1U);
  if (!ascii_alphanumeric(static_cast<unsigned char>(type.front())) ||
      !ascii_alphanumeric(static_cast<unsigned char>(subtype.front())) ||
      !std::all_of(type.begin(), type.end(), media_type_token_byte) ||
      !std::all_of(subtype.begin(), subtype.end(),
                   media_type_token_byte)) {
    fail("model source media_type violates the normalized v1 grammar");
  }
}

void validate_source_symbol(std::string_view symbol,
                            std::string_view field_name) {
  if (symbol.empty()) {
    fail(std::string(field_name) + " contains an empty source symbol");
  }
  utf8proc_ssize_t offset = 0;
  std::size_t code_points = 0U;
  while (offset < static_cast<utf8proc_ssize_t>(symbol.size())) {
    utf8proc_int32_t code_point = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(symbol.data()) + offset,
        static_cast<utf8proc_ssize_t>(symbol.size()) - offset,
        &code_point);
    if (consumed <= 0) {
      fail(std::string(field_name) + " contains invalid UTF-8");
    }
    const auto category = utf8proc_category(code_point);
    if (code_point == 0 || category == UTF8PROC_CATEGORY_CC ||
        category == UTF8PROC_CATEGORY_CF ||
        category == UTF8PROC_CATEGORY_ZS ||
        category == UTF8PROC_CATEGORY_ZL ||
        category == UTF8PROC_CATEGORY_ZP) {
      fail(std::string(field_name) +
           " contains whitespace, control, or format code points");
    }
    offset += consumed;
    ++code_points;
  }
  if (code_points > core::kMaxModelAssetSourceSymbolCodePoints) {
    fail(std::string(field_name) + " exceeds the source-symbol bound");
  }
}

[[nodiscard]] core::ModelAssetInstanceCardinality decode_cardinality(
    const CanonicalValue& value) {
  const auto& token = require_string(value, "instance_cardinality");
  if (token == "one_per_cell") {
    return core::ModelAssetInstanceCardinality::kOnePerCell;
  }
  if (token == "multiple_per_cell") {
    return core::ModelAssetInstanceCardinality::kMultiplePerCell;
  }
  fail("model-asset normalized instance_cardinality is unknown");
}

[[nodiscard]] std::vector<core::ModelSourceArtifactDTO> decode_artifacts(
    const CanonicalValue& value, const core::ModelAssetId& model_asset_id) {
  const auto& source = require_array(value, "expanded_source.artifacts");
  std::vector<core::ModelSourceArtifactDTO> artifacts;
  artifacts.reserve(source.size());
  std::uint64_t aggregate_size = 0U;
  for (const auto& item : source) {
    const auto& logical_uri =
        require_string(require_member(item, "logical_uri"), "logical_uri");
    const auto& media_type =
        require_string(require_member(item, "media_type"), "media_type");
    validate_logical_uri(logical_uri, model_asset_id);
    validate_media_type(media_type);
    const auto byte_size =
        require_byte_size(require_member(item, "byte_size"), "byte_size");
    if (byte_size > core::kMaxModelAssetByteSize - aggregate_size) {
      fail("model source aggregate byte_size exceeds the v1 safe integer");
    }
    aggregate_size += byte_size;
    artifacts.push_back(core::ModelSourceArtifactDTO{
        logical_uri, media_type, byte_size,
        core::ContentDigest(require_string(
            require_member(item, "content_digest"), "content_digest"))});
  }
  return artifacts;
}

[[nodiscard]] core::ExpandedModelSourceDTO decode_expanded_source(
    const CanonicalValue& value, const core::ModelAssetId& model_asset_id) {
  const auto& urdf_logical_uri = require_string(
      require_member(value, "urdf_logical_uri"), "urdf_logical_uri");
  validate_logical_uri(urdf_logical_uri, model_asset_id);

  std::optional<std::string> srdf_logical_uri;
  if (const auto* srdf = value.find_object_member("srdf_logical_uri");
      srdf != nullptr) {
    srdf_logical_uri = require_string(*srdf, "srdf_logical_uri");
    validate_logical_uri(*srdf_logical_uri, model_asset_id);
  }

  return core::ExpandedModelSourceDTO{
      urdf_logical_uri, std::move(srdf_logical_uri),
      decode_artifacts(require_member(value, "artifacts"), model_asset_id)};
}

[[nodiscard]] std::vector<core::ModelFrameRoleBindingDTO>
decode_frame_bindings(const CanonicalValue& value) {
  const auto& source = require_array(value, "frame_role_bindings");
  std::vector<core::ModelFrameRoleBindingDTO> bindings;
  bindings.reserve(source.size());
  for (const auto& item : source) {
    const auto& source_link_name = require_string(
        require_member(item, "source_link_name"), "source_link_name");
    validate_source_symbol(source_link_name, "source_link_name");
    bindings.push_back(core::ModelFrameRoleBindingDTO{
        core::FrameRoleId(require_string(
            require_member(item, "frame_role_id"), "frame_role_id")),
        source_link_name});
  }
  return bindings;
}

[[nodiscard]] std::vector<core::ModelGroupRoleBindingDTO>
decode_group_bindings(const CanonicalValue& value) {
  const auto& source = require_array(value, "model_group_role_bindings");
  std::vector<core::ModelGroupRoleBindingDTO> bindings;
  bindings.reserve(source.size());
  for (const auto& item : source) {
    const auto& source_group_name = require_string(
        require_member(item, "source_group_name"), "source_group_name");
    validate_source_symbol(source_group_name, "source_group_name");
    bindings.push_back(core::ModelGroupRoleBindingDTO{
        core::ModelGroupRoleId(require_string(
            require_member(item, "model_group_role_id"),
            "model_group_role_id")),
        source_group_name});
  }
  return bindings;
}

[[nodiscard]] std::vector<std::string> decode_source_symbols(
    const CanonicalValue& value, std::string_view field_name,
    bool require_unique) {
  const auto& source = require_array(value, field_name);
  std::vector<std::string> symbols;
  symbols.reserve(source.size());
  std::set<std::string, std::less<>> unique;
  for (const auto& item : source) {
    const auto& symbol = require_string(item, field_name);
    validate_source_symbol(symbol, field_name);
    if (require_unique && !unique.insert(symbol).second) {
      fail(std::string(field_name) + " contains a duplicate source symbol");
    }
    symbols.push_back(symbol);
  }
  return symbols;
}

[[nodiscard]] std::vector<core::ModelResourceRoleBindingDTO>
decode_resource_bindings(const CanonicalValue& value) {
  const auto& source = require_array(value, "resource_role_bindings");
  std::vector<core::ModelResourceRoleBindingDTO> bindings;
  bindings.reserve(source.size());
  for (const auto& item : source) {
    bindings.push_back(core::ModelResourceRoleBindingDTO{
        core::ResourceRoleId(require_string(
            require_member(item, "resource_role_id"), "resource_role_id")),
        decode_source_symbols(require_member(item, "source_joint_names"),
                              "source_joint_names", true)});
  }
  return bindings;
}

[[nodiscard]] std::vector<core::ModelCollisionBodyRoleBindingDTO>
decode_collision_bindings(const CanonicalValue& value) {
  const auto& source =
      require_array(value, "collision_body_role_bindings");
  std::vector<core::ModelCollisionBodyRoleBindingDTO> bindings;
  bindings.reserve(source.size());
  for (const auto& item : source) {
    bindings.push_back(core::ModelCollisionBodyRoleBindingDTO{
        core::CollisionBodyRoleId(require_string(
            require_member(item, "collision_body_role_id"),
            "collision_body_role_id")),
        decode_source_symbols(require_member(item, "source_link_names"),
                              "source_link_names", false)});
  }
  return bindings;
}

[[nodiscard]] const core::ModelSourceArtifactDTO* find_artifact(
    const core::ExpandedModelSourceDTO& source, std::string_view logical_uri) {
  const auto found = std::lower_bound(
      source.artifacts.begin(), source.artifacts.end(), logical_uri,
      [](const core::ModelSourceArtifactDTO& artifact,
         std::string_view candidate) {
        return artifact.logical_uri < candidate;
      });
  if (found == source.artifacts.end() ||
      found->logical_uri != logical_uri) {
    return nullptr;
  }
  return &*found;
}

void validate_semantics(const core::ModelAssetDTO& asset) {
  const auto* urdf =
      find_artifact(asset.expanded_source,
                    asset.expanded_source.urdf_logical_uri);
  if (urdf == nullptr || urdf->media_type != "application/xml") {
    fail("urdf_logical_uri must select an exact application/xml artifact");
  }

  if (asset.expanded_source.srdf_logical_uri.has_value()) {
    if (*asset.expanded_source.srdf_logical_uri ==
        asset.expanded_source.urdf_logical_uri) {
      fail("URDF and SRDF logical URIs must be distinct");
    }
    const auto* srdf =
        find_artifact(asset.expanded_source,
                      *asset.expanded_source.srdf_logical_uri);
    if (srdf == nullptr || srdf->media_type != "application/xml") {
      fail("srdf_logical_uri must select an exact application/xml artifact");
    }
  } else if (!asset.model_group_role_bindings.empty()) {
    fail("model-group role bindings require an SRDF entrypoint");
  }

  const auto root_matches = std::count_if(
      asset.frame_role_bindings.begin(), asset.frame_role_bindings.end(),
      [&asset](const core::ModelFrameRoleBindingDTO& binding) {
        return binding.frame_role_id == asset.root_frame_role_id;
      });
  if (root_matches != 1) {
    fail("root_frame_role_id must resolve exactly once in frame bindings");
  }
}

[[nodiscard]] core::ModelAssetDTO decode_normalized(
    const CanonicalValue& normalized) {
  const core::ModelAssetId model_asset_id(
      require_string(require_member(normalized, "model_asset_id"),
                     "model_asset_id"));
  core::ModelAssetDTO decoded{
      model_asset_id,
      core::ModelRevision(require_string(
          require_member(normalized, "model_revision"), "model_revision")),
      decode_cardinality(
          require_member(normalized, "instance_cardinality")),
      decode_expanded_source(require_member(normalized, "expanded_source"),
                             model_asset_id),
      core::FrameRoleId(require_string(
          require_member(normalized, "root_frame_role_id"),
          "root_frame_role_id")),
      decode_frame_bindings(
          require_member(normalized, "frame_role_bindings")),
      decode_group_bindings(
          require_member(normalized, "model_group_role_bindings")),
      decode_resource_bindings(
          require_member(normalized, "resource_role_bindings")),
      decode_collision_bindings(
          require_member(normalized, "collision_body_role_bindings")),
  };
  validate_semantics(decoded);
  return decoded;
}

}  // namespace

QualifiedModelAsset resolve_and_qualify_model_asset(
    const core::ModelAssetCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kModelAssetApiVersion, "model asset");
        } catch (const detail::ExactCatalogReferentError& error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("model_asset"),
      std::string(core::kModelAssetApiVersion), "model_revision"};
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("model_revision does not match normalized semantic content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  if (decoded.model_asset_id != reference.record_id ||
      decoded.model_revision != reference.record_revision) {
    fail("model-asset typed ID/revision selector does not match its referent");
  }
  return QualifiedModelAsset(reference, std::move(decoded),
                             normalized.normalized_root());
}

}  // namespace manyforge::deployment
