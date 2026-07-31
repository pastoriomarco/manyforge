#include "planning_resource_map_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/verified_retained_bundle.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kAssetPrefix = "asset://";
constexpr std::string_view kDigestPrefix = "sha256:";
constexpr std::string_view kStoreKeyPrefix = "sha256/";
constexpr std::string_view kGeneratedNamespace = "mf1";
constexpr std::string_view kGeneratedRevisionSegment = "revision";
constexpr std::string_view kResourceUriDigestDomain =
    "manyforge.planning-resource-uri.mf1";

[[noreturn]] void fail(std::string message) {
  throw RetainedBundleError(std::move(message));
}

[[nodiscard]] bool safe_segment_character(unsigned char byte) noexcept {
  return (byte >= 'A' && byte <= 'Z') ||
         (byte >= 'a' && byte <= 'z') ||
         (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
         byte == '-';
}

void require_safe_path(std::string_view path, std::string_view label) {
  if (path.empty()) {
    fail(std::string(label) + " requires a non-empty path");
  }
  std::size_t offset = 0U;
  while (offset <= path.size()) {
    const auto separator = path.find('/', offset);
    const auto segment =
        path.substr(offset,
                    separator == std::string_view::npos
                        ? path.size() - offset
                        : separator - offset);
    if (segment.empty() || segment.size() > 128U ||
        !safe_segment_character(
            static_cast<unsigned char>(segment.front())) ||
        !std::all_of(segment.begin(), segment.end(),
                     safe_segment_character)) {
      fail(std::string(label) + " contains an unsafe path segment");
    }
    if (separator == std::string_view::npos) {
      break;
    }
    offset = separator + 1U;
  }
}

[[nodiscard]] std::string_view require_digest_hex(
    std::string_view digest, std::string_view label) {
  if (!is_sha256_digest(digest)) {
    fail(std::string(label) +
         " must be an exact lowercase sha256 digest");
  }
  return digest.substr(kDigestPrefix.size());
}

[[nodiscard]] std::string source_relative_path(
    const core::ModelAssetId& model_asset_id,
    std::string_view source_logical_uri) {
  const std::string prefix =
      std::string(kAssetPrefix) + model_asset_id.value() + "/";
  if (!source_logical_uri.starts_with(prefix) ||
      source_logical_uri.size() >
          core::kMaxModelAssetLogicalUriBytes) {
    fail("planning resource source URI is outside its ModelAsset authority");
  }
  const std::string path(source_logical_uri.substr(prefix.size()));
  require_safe_path(path, "planning resource source URI");
  return path;
}

[[nodiscard]] auto row_key(const PlanningResourceMapRow& row) {
  return std::tie(row.model_asset_id, row.model_revision,
                  row.source_logical_uri);
}

void require_row(const PlanningResourceMapRow& row) {
  if (row.media_type.empty() ||
      row.media_type.size() > core::kMaxModelAssetMediaTypeBytes ||
      row.byte_size == 0U ||
      row.byte_size > core::kMaxModelAssetByteSize) {
    fail("planning resource mapping has invalid source metadata");
  }
  const auto digest_hex =
      require_digest_hex(row.content_digest.value(), "content_digest");
  if (row.content_addressed_store_key.value() !=
      std::string(kStoreKeyPrefix) + std::string(digest_hex)) {
    fail("planning resource mapping store key is not digest-derived");
  }
  const std::string expected = compiled_planning_resource_uri(
      row.model_asset_id, row.model_revision, row.source_logical_uri);
  if (row.compiled_logical_uri != expected) {
    fail("planning resource mapping contains a non-compiler URI");
  }
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view label) {
  const auto* result =
      std::get_if<CanonicalValue::Object>(&value.storage());
  if (result == nullptr) {
    fail(std::string(label) + " must be an object");
  }
  return *result;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view label) {
  const auto* result =
      std::get_if<CanonicalValue::Array>(&value.storage());
  if (result == nullptr) {
    fail(std::string(label) + " must be an array");
  }
  return *result;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view label) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& row) { return row.first == name; });
  if (found == object.end()) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return found->second;
}

void require_shape(const CanonicalValue::Object& object,
                   std::initializer_list<std::string_view> expected,
                   std::string_view label) {
  if (object.size() != expected.size()) {
    fail(std::string(label) + " has an invalid closed shape");
  }
  for (const auto name : expected) {
    (void)require_member(object, name, label);
  }
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view label) {
  const auto* result = std::get_if<std::string>(&value.storage());
  if (result == nullptr) {
    fail(std::string(label) + " must be a string");
  }
  return *result;
}

[[nodiscard]] std::uint64_t require_byte_size(
    const CanonicalValue& value, std::string_view label) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 1.0 ||
      std::floor(*number) != *number ||
      *number >
          static_cast<double>(core::kMaxModelAssetByteSize)) {
    fail(std::string(label) + " must be a positive I-JSON-safe integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] CanonicalValue row_projection(
    const PlanningResourceMapRow& row) {
  return CanonicalValue(CanonicalValue::Object{
      {"model_asset_id", CanonicalValue(row.model_asset_id.value())},
      {"model_revision", CanonicalValue(row.model_revision.value())},
      {"source_logical_uri", CanonicalValue(row.source_logical_uri)},
      {"compiled_logical_uri", CanonicalValue(row.compiled_logical_uri)},
      {"media_type", CanonicalValue(row.media_type)},
      {"byte_size", CanonicalValue(static_cast<double>(row.byte_size))},
      {"content_digest", CanonicalValue(row.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(row.content_addressed_store_key.value())},
  });
}

}  // namespace

std::string compiled_planning_resource_uri(
    const core::ModelAssetId& model_asset_id,
    const core::ModelRevision& model_revision,
    std::string_view source_logical_uri) {
  require_retained_portable_identity(
      model_asset_id.value(), "planning resource model_asset_id");
  const std::string relative =
      source_relative_path(model_asset_id, source_logical_uri);
  const auto separator = relative.rfind('/');
  const std::string_view basename =
      separator == std::string::npos
          ? std::string_view(relative)
          : std::string_view(relative).substr(separator + 1U);
  const std::string_view revision_hex =
      require_digest_hex(model_revision.value(), "model_revision");

  std::string preimage(kResourceUriDigestDomain);
  preimage.push_back('\0');
  preimage.append(model_asset_id.value());
  preimage.push_back('\0');
  preimage.append(model_revision.value());
  preimage.push_back('\0');
  preimage.append(source_logical_uri);
  const std::string digest = sha256_digest(preimage);
  const std::string_view source_uri_hex =
      require_digest_hex(digest, "planning resource URI digest");

  std::string result =
      std::string(kAssetPrefix) + model_asset_id.value() + "/" +
      std::string(kGeneratedNamespace) + "/" +
      std::string(kGeneratedRevisionSegment) + "/" +
      std::string(revision_hex) + "/" + std::string(source_uri_hex) + "/" +
      std::string(basename);
  if (result.size() > core::kMaxModelAssetLogicalUriBytes) {
    fail("compiled planning resource URI exceeds the fixed URI bound");
  }
  return result;
}

std::string emit_planning_resource_map(
    std::vector<PlanningResourceMapRow> rows) {
  if (rows.size() > core::kMaxModelAssetCollectionItems) {
    fail("planning resource mapping exceeds the global row bound");
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& left, const auto& right) {
              return row_key(left) < row_key(right);
            });
  std::set<std::string, std::less<>> compiled_uris;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    require_row(rows[index]);
    if ((index != 0U &&
         row_key(rows[index - 1U]) == row_key(rows[index])) ||
        !compiled_uris.insert(rows[index].compiled_logical_uri).second) {
      fail("planning resource mapping contains a duplicate identity");
    }
  }

  CanonicalValue::Array resources;
  resources.reserve(rows.size());
  for (const auto& row : rows) {
    resources.push_back(row_projection(row));
  }
  return canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kPlanningResourceMapApiVersion))},
      {"resources", CanonicalValue(std::move(resources))},
  }));
}

std::vector<PlanningResourceMapRow>
parse_and_verify_planning_resource_map(
    std::string_view canonical_bytes) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() > kMaxArtifactEncodedBytes) {
    fail("planning resource mapping violates the artifact byte bound");
  }
  const auto parsed = parse_strict_json_model_yaml(canonical_bytes);
  if (canonical_json(parsed.root()) != canonical_bytes) {
    fail("planning resource mapping is not exact canonical JSON");
  }
  const auto& root =
      require_object(parsed.root(), "planning resource mapping");
  require_shape(root, {"api_version", "resources"},
                "planning resource mapping");
  if (require_string(
          require_member(root, "api_version",
                         "planning resource mapping"),
          "planning resource mapping api_version") !=
      kPlanningResourceMapApiVersion) {
    fail("planning resource mapping has an unsupported API version");
  }
  const auto& values = require_array(
      require_member(root, "resources", "planning resource mapping"),
      "planning resource mapping resources");
  if (values.size() > core::kMaxModelAssetCollectionItems) {
    fail("planning resource mapping exceeds the global row bound");
  }

  std::vector<PlanningResourceMapRow> rows;
  rows.reserve(values.size());
  std::set<std::string, std::less<>> compiled_uris;
  for (std::size_t index = 0U; index < values.size(); ++index) {
    const std::string label =
        "planning resource mapping resources[" +
        std::to_string(index) + "]";
    const auto& object = require_object(values[index], label);
    require_shape(
        object,
        {"model_asset_id", "model_revision", "source_logical_uri",
         "compiled_logical_uri", "media_type", "byte_size",
         "content_digest", "content_addressed_store_key"},
        label);
    PlanningResourceMapRow row{
        core::ModelAssetId(require_string(
            require_member(object, "model_asset_id", label),
            label + ".model_asset_id")),
        core::ModelRevision(require_string(
            require_member(object, "model_revision", label),
            label + ".model_revision")),
        require_string(
            require_member(object, "source_logical_uri", label),
            label + ".source_logical_uri"),
        require_string(
            require_member(object, "compiled_logical_uri", label),
            label + ".compiled_logical_uri"),
        require_string(require_member(object, "media_type", label),
                       label + ".media_type"),
        require_byte_size(require_member(object, "byte_size", label),
                          label + ".byte_size"),
        core::ContentDigest(require_string(
            require_member(object, "content_digest", label),
            label + ".content_digest")),
        core::ContentAddressedStoreKey(require_string(
            require_member(object, "content_addressed_store_key", label),
            label + ".content_addressed_store_key")),
    };
    require_row(row);
    if ((!rows.empty() && !(row_key(rows.back()) < row_key(row))) ||
        !compiled_uris.insert(row.compiled_logical_uri).second) {
      fail("planning resource mapping is not canonical and unique");
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace manyforge::deployment::detail
