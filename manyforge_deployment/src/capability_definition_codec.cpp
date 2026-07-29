#include "manyforge_deployment/capability_definition_codec.hpp"

#include <manyforge_core/types/catalog.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw CapabilityDefinitionCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue &require_member(const CanonicalValue &object,
                                                   std::string_view name) {
  const auto *member = object.find_object_member(name);
  if (member == nullptr) {
    fail("capability definition normalized value is missing " +
         std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view name) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("capability definition normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] core::CapabilityScope parse_scope(std::string_view value) {
  if (value == "cell_global") {
    return core::CapabilityScope::kCellGlobal;
  }
  if (value == "control_domain") {
    return core::CapabilityScope::kControlDomain;
  }
  if (value == "target_binding") {
    return core::CapabilityScope::kTargetBinding;
  }
  fail("capability definition normalized scope is outside the closed enum");
}

[[nodiscard]] core::CapabilityProviderBindingKind
parse_provider_binding_kind(std::string_view value) {
  if (value == "capability_provider_binding") {
    return core::CapabilityProviderBindingKind::kCapabilityProviderBinding;
  }
  if (value == "control_binding") {
    return core::CapabilityProviderBindingKind::kControlBinding;
  }
  fail("capability definition normalized provider_binding_kind is outside the "
       "closed enum");
}

[[nodiscard]] core::ValueSchemaRefDTO
decode_value_schema_ref(const CanonicalValue &value) {
  return core::ValueSchemaRefDTO{
      core::SchemaId(
          require_string(require_member(value, "schema_id"), "schema_id")),
      core::SchemaRevision(require_string(
          require_member(value, "schema_revision"), "schema_revision")),
  };
}

[[nodiscard]] core::CapabilityDefinitionDTO
decode_normalized(const CanonicalValue &normalized) {
  std::optional<core::ValueSchemaRefDTO> parameters_schema_ref;
  if (normalized.find_object_member("requirement_parameters_schema_ref") !=
      nullptr) {
    parameters_schema_ref = decode_value_schema_ref(
        require_member(normalized, "requirement_parameters_schema_ref"));
  }
  return {
      core::CapabilityId(require_string(
          require_member(normalized, "capability_id"), "capability_id")),
      core::CapabilityDefinitionRevision(require_string(
          require_member(normalized, "capability_definition_revision"),
          "capability_definition_revision")),
      parse_scope(require_string(require_member(normalized, "scope"), "scope")),
      parse_provider_binding_kind(
          require_string(require_member(normalized, "provider_binding_kind"),
                         "provider_binding_kind")),
      std::move(parameters_schema_ref),
  };
}

struct DecodedCapabilityDefinition final {
  core::CapabilityDefinitionDTO value;
  CanonicalValue normalized_root;
};

[[nodiscard]] DecodedCapabilityDefinition
decode_and_match_capability_definition(const ParsedArtifactCandidate &parsed) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *family = registry.find_catalog_schema_family(
      core::CatalogRecordKind::kCapabilityDefinition);
  if (family == nullptr ||
      family->qualification() != CatalogSchemaQualification::kQualified ||
      family->compiled_schema() == nullptr) {
    fail("capability-definition schema family is not build-qualified");
  }

  const auto normalized = validate_and_normalize_unqualified_artifact_candidate(
      *family->compiled_schema(), parsed);
  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("capability_definition"),
      std::string(core::kCapabilityDefinitionApiVersion),
      "capability_definition_revision",
  };
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("capability_definition_revision does not match normalized semantic "
         "content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  const auto *expected =
      core::find_builtin_capability_definition(decoded.capability_id);
  if (expected == nullptr) {
    fail("capability_id is not present in the build-owned normative registry");
  }
  if (decoded != core::materialize_capability_definition(*expected)) {
    fail("capability definition does not match its build-owned normative "
         "registry row");
  }
  if (decoded.requirement_parameters_schema_ref.has_value() &&
      registry.find_qualified_value_schema(
          decoded.requirement_parameters_schema_ref->schema_id,
          decoded.requirement_parameters_schema_ref->schema_revision) ==
          nullptr) {
    fail("capability definition selects an unqualified requirement-parameter "
         "schema");
  }
  return {std::move(decoded), normalized.normalized_root()};
}

} // namespace

QualifiedCapabilityDefinition resolve_and_qualify_capability_definition(
    const core::CapabilityDefinitionCatalogRecordRefDTO &reference,
    const core::ContentAddressedStoreKey &resolved_store_key,
    std::string_view stored_bytes) {
  if (stored_bytes.size() > kMaxArtifactEncodedBytes) {
    fail("capability-definition stored bytes exceed the fixed artifact limit");
  }
  if (reference.content_addressed_store_key.empty() ||
      reference.content_addressed_store_key.value().size() >
          kMaxArtifactScalarBytes ||
      resolved_store_key.empty() ||
      resolved_store_key.value().size() > kMaxArtifactScalarBytes) {
    fail("capability-definition content-addressed store key is outside v1 "
         "bounds");
  }
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *family = registry.find_qualified_catalog_schema(
      core::CatalogRecordKind::kCapabilityDefinition, reference.schema_id,
      reference.schema_revision);
  if (family == nullptr) {
    fail("capability-definition catalog reference selects an unqualified "
         "schema");
  }
  if (reference.schema_id.value() != core::kCapabilityDefinitionApiVersion) {
    fail("capability-definition catalog reference selects the wrong schema "
         "family");
  }
  if (reference.content_addressed_store_key != resolved_store_key) {
    fail("capability-definition bytes were resolved under a different store "
         "key");
  }
  if (reference.content_digest.value() != sha256_digest(stored_bytes)) {
    fail("capability-definition exact stored-byte digest does not match");
  }

  auto decoded = decode_and_match_capability_definition(
      parse_strict_json_model_yaml(stored_bytes));
  if (decoded.value.capability_id != reference.record_id ||
      decoded.value.capability_definition_revision !=
          reference.record_revision) {
    fail("capability-definition typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedCapabilityDefinition(std::move(decoded.value),
                                       std::move(decoded.normalized_root));
}

} // namespace manyforge::deployment
