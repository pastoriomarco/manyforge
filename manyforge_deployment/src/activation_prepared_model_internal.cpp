#include "activation_prepared_model_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>

#include "content_identity_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument("Activation prepared model " +
                              std::move(message));
}

[[nodiscard]] const CanonicalValue::Object &object(const CanonicalValue &value,
                                                   std::string_view label) {
  const auto *result = std::get_if<CanonicalValue::Object>(&value.storage());
  if (result == nullptr) {
    fail(std::string(label) + " must be an object");
  }
  return *result;
}

[[nodiscard]] const CanonicalValue::Array &array(const CanonicalValue &value,
                                                 std::string_view label) {
  const auto *result = std::get_if<CanonicalValue::Array>(&value.storage());
  if (result == nullptr) {
    fail(std::string(label) + " must be an array");
  }
  return *result;
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view label) {
  static_cast<void>(object(value, label));
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return *result;
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view label) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr || result->empty()) {
    fail(std::string(label) + " must be non-empty text");
  }
  return *result;
}

[[nodiscard]] std::string pretty_json(std::string_view canonical) {
  std::string result;
  result.reserve(canonical.size() + canonical.size() / 4U);
  std::size_t indentation = 0U;
  bool in_string = false;
  bool escaped = false;
  const auto newline = [&result, &indentation] {
    result.push_back('\n');
    result.append(indentation * 2U, ' ');
  };

  for (std::size_t index = 0U; index < canonical.size(); ++index) {
    const char byte = canonical[index];
    if (in_string) {
      result.push_back(byte);
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        in_string = false;
      }
      continue;
    }
    if (byte == '"') {
      in_string = true;
      result.push_back(byte);
      continue;
    }
    switch (byte) {
    case '{':
    case '[': {
      result.push_back(byte);
      const char close = byte == '{' ? '}' : ']';
      if (index + 1U < canonical.size() && canonical[index + 1U] != close) {
        ++indentation;
        newline();
      }
      break;
    }
    case '}':
    case ']': {
      const char open = byte == '}' ? '{' : '[';
      if (index > 0U && canonical[index - 1U] != open) {
        --indentation;
        newline();
      }
      result.push_back(byte);
      break;
    }
    case ',':
      result.push_back(byte);
      newline();
      break;
    case ':':
      result.append(": ");
      break;
    default:
      result.push_back(byte);
      break;
    }
  }
  if (in_string || escaped || indentation != 0U) {
    throw std::logic_error("catalog formatter lost structural state");
  }
  result.push_back('\n');
  return result;
}

[[nodiscard]] CanonicalValue
artifact_rows(const std::vector<PreparedModelSourceArtifact> &artifacts) {
  CanonicalValue::Array rows;
  rows.reserve(artifacts.size());
  for (const auto &artifact : artifacts) {
    rows.emplace_back(CanonicalValue::Object{
        {"logical_uri", CanonicalValue(artifact.logical_uri)},
        {"media_type", CanonicalValue(artifact.media_type)},
        {"byte_size",
         CanonicalValue(static_cast<double>(artifact.bytes.size()))},
        {"content_digest", CanonicalValue(artifact.content_digest.value())},
    });
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue normalized_model_asset_candidate(
    std::string_view model_asset_id, const CanonicalValue &semantics,
    const std::vector<PreparedModelSourceArtifact> &artifacts, bool has_srdf) {
  const std::string urdf_uri =
      "asset://" + std::string(model_asset_id) + "/model.urdf";
  const std::string srdf_uri =
      "asset://" + std::string(model_asset_id) + "/model.srdf";
  CanonicalValue::Object candidate{
      {"api_version", CanonicalValue(std::string(core::kModelAssetApiVersion))},
      {"model_asset_id", CanonicalValue(std::string(model_asset_id))},
      {"model_revision", CanonicalValue(std::string(kZeroDigest))},
      {"instance_cardinality",
       member(semantics, "instance_cardinality", "semantics")},
      {"expanded_source",
       CanonicalValue(CanonicalValue::Object{
           {"urdf_logical_uri", CanonicalValue(urdf_uri)},
           {"srdf_logical_uri",
            has_srdf ? CanonicalValue(srdf_uri) : CanonicalValue(nullptr)},
           {"artifacts", artifact_rows(artifacts)},
       })},
      {"root_frame_role_id",
       member(semantics, "root_frame_role_id", "semantics")},
      {"frame_role_bindings",
       member(semantics, "frame_role_bindings", "semantics")},
      {"model_group_role_bindings",
       member(semantics, "model_group_role_bindings", "semantics")},
      {"resource_role_bindings",
       member(semantics, "resource_role_bindings", "semantics")},
      {"collision_body_role_bindings",
       member(semantics, "collision_body_role_bindings", "semantics")},
  };
  // A family's own payload (a tool's mass and centre of mass) is optional.
  if (const auto *payload = semantics.find_object_member("payload");
      payload != nullptr) {
    candidate.emplace_back("payload", *payload);
  }

  const auto *schema =
      build_qualified_closed_schema_registry().find_catalog_schema_family(
          core::CatalogRecordKind::kModelAsset);
  if (schema == nullptr ||
      schema->qualification() != CatalogSchemaQualification::kQualified ||
      schema->compiled_schema() == nullptr) {
    throw std::logic_error("model-asset schema is not build-qualified");
  }
  return validate_and_normalize_unqualified_artifact_candidate(
             *schema->compiled_schema(),
             parse_strict_json_model_yaml(
                 canonical_json(CanonicalValue(std::move(candidate)))))
      .normalized_root();
}

[[nodiscard]] std::string source_store_key(std::string_view digest) {
  if (!is_sha256_digest(digest)) {
    fail("source artifact does not carry one SHA-256 identity");
  }
  return "sha256/" + std::string(digest.substr(7U));
}

} // namespace

ActivationPreparedModel qualify_activation_prepared_model(
    std::string_view model_asset_id, const CanonicalValue &semantics,
    std::string_view expected_package_uri_prefix,
    std::string_view canonical_preparation_receipt,
    std::string_view original_urdf_bytes,
    std::optional<std::string_view> original_srdf_bytes,
    std::vector<PreparedModelSourceArtifact> artifacts) {
  const auto receipt =
      parse_strict_json_model_yaml(canonical_preparation_receipt);
  const auto &receipt_root = receipt.root();
  if (canonical_json(receipt_root) != canonical_preparation_receipt) {
    fail("preparation receipt is not canonical JSON");
  }
  if (text(member(receipt_root, "accepted_package_uri_prefix",
                  "preparation receipt"),
           "preparation receipt.accepted_package_uri_prefix") !=
      expected_package_uri_prefix) {
    fail("preparation receipt selects a different package URI prefix");
  }

  if (artifacts.empty() ||
      artifacts.size() > core::kMaxModelAssetCollectionItems) {
    fail("source artifact count is outside the compiler bound");
  }
  std::sort(artifacts.begin(), artifacts.end(),
            [](const auto &left, const auto &right) {
              return left.logical_uri < right.logical_uri;
            });
  std::set<std::string, std::less<>> logical_uris;
  std::uint64_t aggregate_bytes = 0U;
  for (const auto &artifact : artifacts) {
    if (artifact.logical_uri.empty() || artifact.media_type.empty() ||
        artifact.bytes.empty() ||
        !logical_uris.insert(artifact.logical_uri).second) {
      fail("source artifacts are empty or contain duplicate identities");
    }
    if (!is_sha256_digest(artifact.content_digest.value()) ||
        sha256_digest(artifact.bytes) != artifact.content_digest.value()) {
      fail("source artifact bytes differ from their exact identity");
    }
    if (artifact.bytes.size() > kMaxModelSourceClosureBytes - aggregate_bytes) {
      fail("source closure exceeds the compiler-owned byte bound");
    }
    aggregate_bytes += static_cast<std::uint64_t>(artifact.bytes.size());
  }

  auto normalized = normalized_model_asset_candidate(
      model_asset_id, semantics, artifacts, original_srdf_bytes.has_value());
  const ContentIdentitySpec identity_spec{
      catalog_record_identity_domain("model_asset"),
      std::string(core::kModelAssetApiVersion), "model_revision"};
  const std::string revision =
      compute_content_identity(normalized, identity_spec);
  auto root = normalized.as_object();
  auto own_field = std::find_if(root.begin(), root.end(), [](const auto &item) {
    return item.first == "model_revision";
  });
  if (own_field == root.end()) {
    throw std::logic_error("normalized ModelAsset lost model_revision");
  }
  own_field->second = CanonicalValue(revision);
  const std::string catalog_bytes =
      pretty_json(canonical_json(CanonicalValue(std::move(root))));

  const auto *schema =
      build_qualified_closed_schema_registry().find_catalog_schema_family(
          core::CatalogRecordKind::kModelAsset);
  if (schema == nullptr || schema->compiled_schema() == nullptr) {
    throw std::logic_error("model-asset schema disappeared during generation");
  }
  const core::ContentDigest catalog_digest(sha256_digest(catalog_bytes));
  const core::ContentAddressedStoreKey catalog_key("catalog/model-assets/" +
                                                   std::string(model_asset_id) +
                                                   "/" + revision.substr(7U));
  const core::ModelAssetCatalogRecordRefDTO reference{
      core::ModelAssetId(std::string(model_asset_id)),
      core::ModelRevision(revision),
      core::SchemaId(std::string(core::kModelAssetApiVersion)),
      schema->compiled_schema()->schema_revision(),
      catalog_digest,
      catalog_key};
  const auto qualified =
      resolve_and_qualify_model_asset(reference, catalog_key, catalog_bytes);

  std::vector<PreparedModelSourceArtifact> retained_artifacts = artifacts;
  std::vector<ResolvedModelSourceArtifactCandidate> candidates;
  candidates.reserve(artifacts.size());
  for (auto &artifact : artifacts) {
    candidates.push_back({std::move(artifact.logical_uri),
                          core::ContentAddressedStoreKey(source_store_key(
                              artifact.content_digest.value())),
                          std::move(artifact.bytes)});
  }
  auto verified = verify_model_source_closure(qualified, std::move(candidates));
  auto preparation = verify_build_qualified_structural_model_preparation(
      canonical_preparation_receipt, original_urdf_bytes, original_srdf_bytes,
      verified);
  return {catalog_bytes, reference, std::move(retained_artifacts),
          std::move(verified),
          std::move(preparation)};
}

std::vector<ActivationPreparedModel> qualify_activation_prepared_models(
    const ActivationDeploymentIntent &intent,
    const std::vector<ActivationComponentProfile> &profiles,
    std::vector<PreparedComponentModelSource> prepared_sources) {
  const auto physical_count = static_cast<std::size_t>(
      std::count_if(profiles.begin(), profiles.end(), [](const auto &profile) {
        return !is_model_free_runtime_kind(profile.runtime_kind);
      }));
  if (intent.components.size() != profiles.size() ||
      physical_count != prepared_sources.size()) {
    fail("component/profile/prepared sets differ");
  }

  std::map<std::string, PreparedComponentModelSource, std::less<>> prepared;
  for (auto &source : prepared_sources) {
    if (!prepared.emplace(source.component_id, std::move(source)).second) {
      fail("prepared component set contains a duplicate identity");
    }
  }

  std::vector<ActivationPreparedModel> result;
  result.reserve(intent.components.size());
  for (std::size_t index = 0U; index < intent.components.size(); ++index) {
    const auto &component = intent.components[index];
    const auto &profile = profiles[index];
    if (profile.component_id != component.component_id) {
      fail("component profile order differs from authored intent");
    }
    if (is_model_free_runtime_kind(profile.runtime_kind)) {
      if (component.model_source || component.root_pose_in_cell ||
          prepared.contains(component.component_id))
        fail("model-free component cannot carry a prepared model");
      continue;
    }
    if (!component.model_source)
      fail("physical component has no captured model");
    auto found = prepared.find(component.component_id);
    if (found == prepared.end()) {
      fail("authored component has no prepared model");
    }
    auto source = std::move(found->second);
    prepared.erase(found);
    if (source.model_asset_id != profile.model_asset_id ||
        source.accepted_package_uri_prefix !=
            profile.accepted_package_uri_prefix) {
      fail("prepared model differs from its repository-owned profile");
    }

    std::map<std::string, const CapturedModelSourceArtifact *, std::less<>>
        captured_resources;
    for (const auto &resource : component.model_source->resources) {
      if (!captured_resources.emplace(resource.logical_uri, &resource).second) {
        fail("captured component contains duplicate resource identities");
      }
    }

    std::vector<PreparedModelSourceArtifact> artifacts;
    artifacts.push_back({source.urdf.logical_uri, source.urdf.media_type,
                         source.urdf.content_digest,
                         std::move(source.urdf.bytes)});
    if (source.srdf.has_value()) {
      artifacts.push_back({source.srdf->logical_uri, source.srdf->media_type,
                           source.srdf->content_digest,
                           std::move(source.srdf->bytes)});
    }
    const auto &rewrites = array(
        member(source.preparation_receipt, "resource_rewrites",
               "preparation receipt"),
        "preparation receipt.resource_rewrites");
    std::set<std::string, std::less<>> asset_uris;
    for (const auto &rewrite : rewrites) {
      const auto &asset_uri =
          text(member(rewrite, "asset_uri", "resource rewrite"),
               "resource rewrite.asset_uri");
      const auto &original_uri =
          text(member(rewrite, "original_package_uri", "resource rewrite"),
               "resource rewrite.original_package_uri");
      const auto &media_type =
          text(member(rewrite, "media_type", "resource rewrite"),
               "resource rewrite.media_type");
      if (!asset_uris.insert(asset_uri).second) {
        fail("preparation receipt repeats an asset URI");
      }
      const auto captured = captured_resources.find(original_uri);
      if (captured == captured_resources.end()) {
        fail("preparation receipt names an uncaptured resource");
      }
      artifacts.push_back(
          {asset_uri, media_type, captured->second->content_digest,
           captured->second->bytes});
      captured_resources.erase(captured);
    }
    if (!captured_resources.empty()) {
      fail("captured component contains a resource absent from its receipt");
    }

    const auto &semantics =
        member(profile.model_family, "semantics", "model family");
    result.push_back(qualify_activation_prepared_model(
        profile.model_asset_id, semantics, profile.accepted_package_uri_prefix,
        canonical_json(source.preparation_receipt),
        component.model_source->urdf.bytes,
        component.model_source->srdf.has_value()
            ? std::optional<std::string_view>(
                  component.model_source->srdf->bytes)
            : std::nullopt,
        std::move(artifacts)));
  }
  if (!prepared.empty()) {
    fail("prepared model set contains an unused component");
  }
  return result;
}

} // namespace manyforge::deployment::detail
