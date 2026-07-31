#include "manyforge_deployment/structural_model_preparation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_identity_internal.hpp"
#include "embedded_structural_model_preparation.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

using Object = CanonicalValue::Object;
using Array = CanonicalValue::Array;

[[noreturn]] void fail(std::string message) {
  throw StructuralModelPreparationError(std::move(message));
}

[[nodiscard]] const Object& require_object(
    const CanonicalValue& value, std::string_view label) {
  const auto* object = std::get_if<Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(label) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const Array& require_array(
    const CanonicalValue& value, std::string_view label) {
  const auto* array = std::get_if<Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(label) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const CanonicalValue& require_member(
    const Object& object, std::string_view name,
    std::string_view label) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return found->second;
}

void require_shape(
    const Object& object,
    std::initializer_list<std::string_view> expected,
    std::string_view label) {
  if (object.size() != expected.size()) {
    fail(std::string(label) + " has an open or incomplete shape");
  }
  for (const auto name : expected) {
    (void)require_member(object, name, label);
  }
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view label) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(std::string(label) + " must be a string");
  }
  return *text;
}

[[nodiscard]] std::uint64_t require_nonnegative_integer(
    const CanonicalValue& value, std::uint64_t maximum,
    std::string_view label) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) ||
      *number < 0.0 || std::floor(*number) != *number ||
      *number > static_cast<double>(maximum)) {
    fail(std::string(label) +
         " must be one bounded non-negative I-JSON integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] bool portable_character(char value) noexcept {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9') || value == '.' ||
         value == '_' || value == '-';
}

[[nodiscard]] bool alphanumeric(char value) noexcept {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9');
}

void require_safe_segment(std::string_view segment,
                          std::string_view label) {
  if (segment.empty() || segment.size() > 128U ||
      !alphanumeric(segment.front()) ||
      !std::all_of(segment.begin(), segment.end(),
                   portable_character)) {
    fail(std::string(label) + " contains an unsafe path segment");
  }
}

void require_safe_relative_path(std::string_view path,
                                std::string_view label) {
  if (path.empty() || path.size() > 2048U ||
      path.front() == '/' || path.back() == '/' ||
      path.find('\\') != std::string_view::npos) {
    fail(std::string(label) + " is not one safe relative path");
  }
  std::size_t offset = 0U;
  while (offset < path.size()) {
    const std::size_t separator = path.find('/', offset);
    const auto segment =
        path.substr(offset, separator == std::string_view::npos
                                ? path.size() - offset
                                : separator - offset);
    require_safe_segment(segment, label);
    if (separator == std::string_view::npos) {
      break;
    }
    offset = separator + 1U;
  }
}

void require_package_prefix(std::string_view prefix) {
  constexpr std::string_view kScheme = "package://";
  if (!prefix.starts_with(kScheme) || !prefix.ends_with('/') ||
      prefix.size() > 2048U) {
    fail("accepted_package_uri_prefix is not one bounded package prefix");
  }
  require_safe_relative_path(
      prefix.substr(kScheme.size(),
                    prefix.size() - kScheme.size() - 1U),
      "accepted_package_uri_prefix");
}

void require_exact_raw_identity(
    const CanonicalValue& candidate, std::string_view bytes,
    std::string_view label) {
  const auto& object = require_object(candidate, label);
  require_shape(object, {"byte_size", "content_digest"}, label);
  const auto size = require_nonnegative_integer(
      require_member(object, "byte_size", label),
      static_cast<std::uint64_t>(kMaxArtifactSafeInteger), label);
  const auto& digest = require_string(
      require_member(object, "content_digest", label), label);
  if (size != static_cast<std::uint64_t>(bytes.size()) ||
      !is_sha256_digest(digest) || sha256_digest(bytes) != digest) {
    fail(std::string(label) +
         " does not identify the exact supplied bytes");
  }
}

[[nodiscard]] std::string_view expected_resource_kind(
    ModelSourceArtifactKind kind) {
  switch (kind) {
    case ModelSourceArtifactKind::kStlMesh:
    case ModelSourceArtifactKind::kColladaMesh:
      return "mesh";
    case ModelSourceArtifactKind::kPngTexture:
    case ModelSourceArtifactKind::kJpegTexture:
      return "texture";
    case ModelSourceArtifactKind::kUrdfXmlEntrypoint:
    case ModelSourceArtifactKind::kSrdfXmlEntrypoint:
      break;
  }
  fail("an XML entrypoint cannot be a resource rewrite");
}

struct ReceiptArtifact final {
  std::string logical_uri;
  std::string media_type;
  std::uint64_t byte_size;
  core::ContentDigest content_digest;
};

struct SourceReferenceOccurrences final {
  std::string_view kind;
  std::uint64_t count;
};

void add_source_reference(
    std::map<std::string, SourceReferenceOccurrences, std::less<>>&
        occurrences,
    std::string_view logical_uri, std::string_view kind) {
  const auto found = occurrences.find(logical_uri);
  if (found == occurrences.end()) {
    occurrences.emplace(
        logical_uri, SourceReferenceOccurrences{kind, 1U});
    return;
  }
  if (found->second.kind != kind ||
      found->second.count ==
          std::numeric_limits<std::uint64_t>::max()) {
    fail("verified source contains an ambiguous or overflowing resource "
         "occurrence");
  }
  ++found->second.count;
}

void add_geometry_reference(
    std::map<std::string, SourceReferenceOccurrences, std::less<>>&
        occurrences,
    const UrdfGeometryIr& geometry) {
  const auto* mesh = std::get_if<UrdfMeshGeometryIr>(&geometry);
  if (mesh != nullptr) {
    add_source_reference(occurrences, mesh->logical_uri, "mesh");
  }
}

void add_material_reference(
    std::map<std::string, SourceReferenceOccurrences, std::less<>>&
        occurrences,
    const UrdfMaterialIr& material) {
  if (material.texture.has_value()) {
    add_source_reference(
        occurrences, material.texture->logical_uri, "texture");
  }
}

[[nodiscard]] std::map<
    std::string, SourceReferenceOccurrences, std::less<>>
source_reference_occurrences(
    const SourceVerifiedModelAsset& verified_model_asset) {
  std::map<std::string, SourceReferenceOccurrences, std::less<>>
      occurrences;
  const auto& urdf = verified_model_asset.source_ir().urdf();
  for (const auto& material : urdf.materials) {
    add_material_reference(occurrences, material);
  }
  for (const auto& link : urdf.links) {
    for (const auto& visual : link.visuals) {
      add_geometry_reference(occurrences, visual.geometry);
      if (visual.material.has_value()) {
        add_material_reference(occurrences, *visual.material);
      }
    }
    for (const auto& collision : link.collisions) {
      add_geometry_reference(occurrences, collision.geometry);
    }
  }
  return occurrences;
}

[[nodiscard]] ReceiptArtifact parse_artifact(
    const CanonicalValue& candidate) {
  const auto& object =
      require_object(candidate, "prepared_source.artifact");
  require_shape(
      object,
      {"logical_uri", "media_type", "byte_size", "content_digest"},
      "prepared_source.artifact");
  const auto& logical_uri = require_string(
      require_member(object, "logical_uri",
                     "prepared_source.artifact"),
      "prepared_source.artifact.logical_uri");
  const auto& media_type = require_string(
      require_member(object, "media_type",
                     "prepared_source.artifact"),
      "prepared_source.artifact.media_type");
  const auto byte_size = require_nonnegative_integer(
      require_member(object, "byte_size",
                     "prepared_source.artifact"),
      static_cast<std::uint64_t>(kMaxArtifactSafeInteger),
      "prepared_source.artifact.byte_size");
  const auto& digest = require_string(
      require_member(object, "content_digest",
                     "prepared_source.artifact"),
      "prepared_source.artifact.content_digest");
  if (byte_size == 0U || !is_sha256_digest(digest)) {
    fail("prepared_source.artifact has an invalid size or digest");
  }
  return {logical_uri, media_type, byte_size,
          core::ContentDigest(digest)};
}

void verify_prepared_artifacts(
    const Object& prepared_source,
    const SourceVerifiedModelAsset& verified_model_asset) {
  require_shape(
      prepared_source,
      {"artifacts", "srdf_logical_uri", "urdf_logical_uri"},
      "prepared_source");
  const auto& asset = verified_model_asset.value();
  const auto& urdf_uri = require_string(
      require_member(prepared_source, "urdf_logical_uri",
                     "prepared_source"),
      "prepared_source.urdf_logical_uri");
  if (urdf_uri != asset.expanded_source.urdf_logical_uri) {
    fail("prepared_source selects a different URDF entrypoint");
  }
  const auto& srdf_candidate = require_member(
      prepared_source, "srdf_logical_uri", "prepared_source");
  if (asset.expanded_source.srdf_logical_uri.has_value()) {
    const auto& srdf_uri = require_string(
        srdf_candidate, "prepared_source.srdf_logical_uri");
    if (srdf_uri != *asset.expanded_source.srdf_logical_uri) {
      fail("prepared_source selects a different SRDF entrypoint");
    }
  } else if (!std::holds_alternative<std::nullptr_t>(
                 srdf_candidate.storage())) {
    fail("prepared_source must record absent SRDF as null");
  }

  const auto& artifact_candidates = require_array(
      require_member(prepared_source, "artifacts",
                     "prepared_source"),
      "prepared_source.artifacts");
  const auto& plan =
      verified_model_asset.retrieval_plan().artifacts();
  const auto& verified = verified_model_asset.verified_artifacts();
  if (artifact_candidates.size() != plan.size() ||
      verified.size() != plan.size()) {
    fail("prepared_source does not have the exact verified closure size");
  }

  std::map<std::string, const ModelSourceRetrievalPlanRow*,
           std::less<>>
      plan_by_uri;
  for (const auto& row : plan) {
    plan_by_uri.emplace(row.logical_uri, &row);
  }
  std::map<std::string, const VerifiedModelSourceArtifact*,
           std::less<>>
      verified_by_uri;
  for (const auto& row : verified) {
    verified_by_uri.emplace(row.logical_uri, &row);
  }

  std::string previous;
  for (const auto& candidate : artifact_candidates) {
    const auto artifact = parse_artifact(candidate);
    if (!previous.empty() && artifact.logical_uri <= previous) {
      fail("prepared_source.artifacts is not canonical-sorted");
    }
    previous = artifact.logical_uri;
    const auto expected = plan_by_uri.find(artifact.logical_uri);
    const auto proof = verified_by_uri.find(artifact.logical_uri);
    if (expected == plan_by_uri.end() ||
        proof == verified_by_uri.end()) {
      fail("prepared_source contains an artifact outside the verified plan");
    }
    const auto& plan_row = *expected->second;
    const auto& verified_row = *proof->second;
    if (artifact.media_type != plan_row.media_type ||
        artifact.byte_size != plan_row.byte_size ||
        artifact.content_digest != plan_row.content_digest ||
        verified_row.media_type != plan_row.media_type ||
        verified_row.byte_size != plan_row.byte_size ||
        verified_row.content_digest != plan_row.content_digest ||
        verified_row.resolved_store_key !=
            plan_row.expected_store_key) {
      fail("prepared_source artifact differs from its independently "
           "verified retrieval-plan row");
    }
  }
}

void verify_resource_rewrites(
    const CanonicalValue& candidate, std::string_view package_prefix,
    const SourceVerifiedModelAsset& verified_model_asset) {
  const auto& rewrites =
      require_array(candidate, "resource_rewrites");
  std::map<std::string, const ModelSourceRetrievalPlanRow*,
           std::less<>>
      dependencies;
  const auto& asset = verified_model_asset.value();
  for (const auto& row :
       verified_model_asset.retrieval_plan().artifacts()) {
    if (row.logical_uri != asset.expanded_source.urdf_logical_uri &&
        (!asset.expanded_source.srdf_logical_uri.has_value() ||
         row.logical_uri !=
             *asset.expanded_source.srdf_logical_uri)) {
      dependencies.emplace(row.logical_uri, &row);
    }
  }
  if (rewrites.size() != dependencies.size()) {
    fail("resource_rewrites does not cover the exact dependency closure");
  }
  const auto occurrences =
      source_reference_occurrences(verified_model_asset);
  if (occurrences.size() != dependencies.size()) {
    fail("verified source occurrence table differs from its dependency "
         "closure");
  }

  std::set<std::string, std::less<>> covered;
  std::string previous;
  for (const auto& rewrite_candidate : rewrites) {
    const auto& rewrite =
        require_object(rewrite_candidate, "resource_rewrite");
    require_shape(
        rewrite,
        {"asset_uri", "byte_size", "content_digest", "kind",
         "media_type", "occurrence_count", "original_package_uri",
         "relative_path"},
        "resource_rewrite");
    const auto& asset_uri = require_string(
        require_member(rewrite, "asset_uri", "resource_rewrite"),
        "resource_rewrite.asset_uri");
    if (!previous.empty() && asset_uri <= previous) {
      fail("resource_rewrites is not canonical-sorted");
    }
    previous = asset_uri;
    const auto expected = dependencies.find(asset_uri);
    if (expected == dependencies.end() ||
        !covered.emplace(asset_uri).second) {
      fail("resource_rewrite invents or duplicates a source dependency");
    }
    const auto& row = *expected->second;
    const auto source_occurrence = occurrences.find(asset_uri);
    if (source_occurrence == occurrences.end()) {
      fail("resource_rewrite has no verified source occurrence");
    }
    const auto byte_size = require_nonnegative_integer(
        require_member(rewrite, "byte_size", "resource_rewrite"),
        static_cast<std::uint64_t>(kMaxArtifactSafeInteger),
        "resource_rewrite.byte_size");
    const auto& digest = require_string(
        require_member(rewrite, "content_digest",
                       "resource_rewrite"),
        "resource_rewrite.content_digest");
    const auto& kind = require_string(
        require_member(rewrite, "kind", "resource_rewrite"),
        "resource_rewrite.kind");
    const auto& media_type = require_string(
        require_member(rewrite, "media_type", "resource_rewrite"),
        "resource_rewrite.media_type");
    const auto occurrence_count = require_nonnegative_integer(
        require_member(rewrite, "occurrence_count",
                       "resource_rewrite"),
        kMaxModelSourceXmlElementCount,
        "resource_rewrite.occurrence_count");
    const auto& original_uri = require_string(
        require_member(rewrite, "original_package_uri",
                       "resource_rewrite"),
        "resource_rewrite.original_package_uri");
    const auto& relative_path = require_string(
        require_member(rewrite, "relative_path",
                       "resource_rewrite"),
        "resource_rewrite.relative_path");
    require_safe_relative_path(relative_path,
                               "resource_rewrite.relative_path");
    if (occurrence_count != source_occurrence->second.count ||
        kind != source_occurrence->second.kind ||
        byte_size != row.byte_size ||
        digest != row.content_digest.value() ||
        media_type != row.media_type ||
        kind != expected_resource_kind(row.artifact_kind) ||
        original_uri !=
            std::string(package_prefix) + relative_path) {
      fail("resource_rewrite differs from its exact verified source row");
    }
  }
  if (covered.size() != dependencies.size()) {
    fail("resource_rewrites omits a verified source dependency");
  }
}

void verify_removed_counts(const CanonicalValue& candidate) {
  const auto& counts =
      require_object(candidate, "removed_top_level_elements");
  require_shape(counts, {"gazebo", "ros2_control", "transmission"},
                "removed_top_level_elements");
  std::uint64_t aggregate = 0U;
  for (const auto name :
       {"gazebo", "ros2_control", "transmission"}) {
    const auto count = require_nonnegative_integer(
        require_member(counts, name, "removed_top_level_elements"),
        kMaxModelSourceXmlElementCount,
        "removed_top_level_elements count");
    if (count >
        kMaxModelSourceXmlElementCount - aggregate) {
      fail("removed_top_level_elements exceeds the XML element bound");
    }
    aggregate += count;
  }
}

}  // namespace

BuildQualifiedStructuralModelPreparation
verify_build_qualified_structural_model_preparation(
    std::string_view canonical_receipt_bytes,
    std::string_view original_urdf_bytes,
    std::optional<std::string_view> original_srdf_bytes,
    const SourceVerifiedModelAsset& verified_model_asset) {
  try {
    const auto parsed =
        parse_strict_json_model_yaml(canonical_receipt_bytes);
    const auto& root = parsed.root();
    const auto& object = require_object(root, "preparation receipt");
    require_shape(
        object,
        {"accepted_package_uri_prefix", "api_version", "build_digest",
         "configuration_digest", "generator_id", "generator_version",
         "model_asset_id", "original_source", "prepared_source",
         "removed_top_level_elements", "resource_rewrites"},
        "preparation receipt");
    if (canonical_json(root) != canonical_receipt_bytes) {
      fail("preparation receipt is not exact RFC 8785 JSON");
    }

    const auto& api_version = require_string(
        require_member(object, "api_version", "preparation receipt"),
        "preparation receipt api_version");
    const auto& generator_id = require_string(
        require_member(object, "generator_id", "preparation receipt"),
        "preparation receipt generator_id");
    const auto& generator_version = require_string(
        require_member(object, "generator_version",
                       "preparation receipt"),
        "preparation receipt generator_version");
    const auto& build_digest = require_string(
        require_member(object, "build_digest", "preparation receipt"),
        "preparation receipt build_digest");
    const auto& configuration_digest = require_string(
        require_member(object, "configuration_digest",
                       "preparation receipt"),
        "preparation receipt configuration_digest");
    if (api_version != kStructuralModelPreparationApiVersion ||
        generator_id != kStructuralModelPreparationGeneratorId ||
        generator_version !=
            kStructuralModelPreparationGeneratorVersion ||
        build_digest !=
            detail::kStructuralModelPreparationGeneratorBuildDigest ||
        !is_sha256_digest(configuration_digest)) {
      fail("preparation receipt does not select the exact build-owned "
           "generator");
    }
    const detail::ContentIdentitySpec identity_spec{
        std::string(
            kStructuralModelPreparationConfigurationIdentityDomain),
        std::string(kStructuralModelPreparationApiVersion),
        "configuration_digest",
        detail::ContentIdentityRootShape::kDocument};
    if (!detail::verify_content_identity(root, identity_spec)) {
      fail("preparation receipt configuration digest does not match its "
           "complete canonical preimage");
    }

    const auto& model_asset_id = require_string(
        require_member(object, "model_asset_id",
                       "preparation receipt"),
        "preparation receipt model_asset_id");
    if (model_asset_id !=
        verified_model_asset.value().model_asset_id.value()) {
      fail("preparation receipt selects a different ModelAsset");
    }
    const auto& package_prefix = require_string(
        require_member(object, "accepted_package_uri_prefix",
                       "preparation receipt"),
        "preparation receipt accepted_package_uri_prefix");
    require_package_prefix(package_prefix);

    const auto& original_source = require_object(
        require_member(object, "original_source",
                       "preparation receipt"),
        "original_source");
    require_shape(original_source, {"srdf", "urdf"},
                  "original_source");
    require_exact_raw_identity(
        require_member(original_source, "urdf", "original_source"),
        original_urdf_bytes, "original_source.urdf");
    const auto& srdf =
        require_member(original_source, "srdf", "original_source");
    if (original_srdf_bytes.has_value() !=
        verified_model_asset.value()
            .expanded_source.srdf_logical_uri.has_value()) {
      fail("original SRDF presence differs from the verified prepared "
           "source");
    }
    if (original_srdf_bytes.has_value()) {
      require_exact_raw_identity(srdf, *original_srdf_bytes,
                                 "original_source.srdf");
    } else if (!std::holds_alternative<std::nullptr_t>(
                   srdf.storage())) {
      fail("original_source must record absent SRDF as null");
    }

    const auto& prepared_source = require_object(
        require_member(object, "prepared_source",
                       "preparation receipt"),
        "prepared_source");
    verify_prepared_artifacts(prepared_source,
                              verified_model_asset);
    verify_resource_rewrites(
        require_member(object, "resource_rewrites",
                       "preparation receipt"),
        package_prefix, verified_model_asset);
    verify_removed_counts(
        require_member(object, "removed_top_level_elements",
                       "preparation receipt"));

    return BuildQualifiedStructuralModelPreparation(
        core::RetainedGeneratorRecordDTO{
            core::GeneratorId(generator_id),
            core::GeneratorVersion(generator_version),
            core::ContentDigest(build_digest),
            core::ContentDigest(configuration_digest),
        },
        verified_model_asset, std::string(canonical_receipt_bytes));
  } catch (const StructuralModelPreparationError&) {
    throw;
  } catch (const std::invalid_argument& error) {
    fail(std::string("invalid structural-model preparation receipt: ") +
         error.what());
  }
}

}  // namespace manyforge::deployment
