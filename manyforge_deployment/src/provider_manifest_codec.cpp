#include "manyforge_deployment/provider_manifest_codec.hpp"

#include <manyforge_core/types/capability_definition.hpp>

#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/closed_schema.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw ProviderManifestCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue& object, std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    fail("provider-manifest normalized value is missing " +
         std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view name) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("provider-manifest normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view name) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("provider-manifest normalized field is not an array: " +
         std::string(name));
  }
  return *array;
}

template <typename StrongId>
[[nodiscard]] std::vector<StrongId> decode_role_ids(
    const CanonicalValue& value, std::string_view field_name) {
  const auto& source = require_array(value, field_name);
  std::vector<StrongId> roles;
  roles.reserve(source.size());
  for (const auto& item : source) {
    roles.emplace_back(require_string(item, field_name));
  }
  return roles;
}

[[nodiscard]] core::ProviderContractRefDTO decode_provider_contract_ref(
    const CanonicalValue& value) {
  return {
      core::ProviderContractRoleId(require_string(
          require_member(value, "provider_contract_role_id"),
          "provider_contract_ref.provider_contract_role_id")),
      core::SchemaId(require_string(
          require_member(value, "contract_schema_id"),
          "provider_contract_ref.contract_schema_id")),
      core::SchemaRevision(require_string(
          require_member(value, "contract_schema_revision"),
          "provider_contract_ref.contract_schema_revision")),
  };
}

[[nodiscard]] core::StopContainmentCommandKind parse_command_kind(
    std::string_view token) {
  if (token == "motion_controlled_stop") {
    return core::StopContainmentCommandKind::kMotionControlledStop;
  }
  if (token == "safe_output_command") {
    return core::StopContainmentCommandKind::kSafeOutputCommand;
  }
  if (token == "logical_cancel") {
    return core::StopContainmentCommandKind::kLogicalCancel;
  }
  if (token == "registered") {
    return core::StopContainmentCommandKind::kRegistered;
  }
  fail("provider-manifest command kind is outside the closed enum");
}

[[nodiscard]] core::StopEvidenceRequirementKind parse_evidence_kind(
    std::string_view token) {
  if (token == "motion_rest") {
    return core::StopEvidenceRequirementKind::kMotionRest;
  }
  if (token == "io_safe_output") {
    return core::StopEvidenceRequirementKind::kIoSafeOutput;
  }
  if (token == "logical_cancellation_no_continuing_effect") {
    return core::StopEvidenceRequirementKind::
        kLogicalCancellationNoContinuingEffect;
  }
  if (token == "registered") {
    return core::StopEvidenceRequirementKind::kRegistered;
  }
  fail("provider-manifest evidence kind is outside the closed enum");
}

[[nodiscard]] core::ProviderStopCommandSupportDTO decode_command_support(
    const CanonicalValue& value) {
  const auto kind = parse_command_kind(require_string(
      require_member(value, "command_kind"), "stop_command_support.command_kind"));
  const auto* registered = value.find_object_member("registered_command_kind_id");
  const bool requires_registered =
      kind == core::StopContainmentCommandKind::kRegistered;
  if ((registered != nullptr) != requires_registered) {
    fail("provider-manifest Stop command has invalid registered-kind presence");
  }

  std::optional<core::RegisteredStopCommandKindId> registered_id;
  if (registered != nullptr) {
    registered_id.emplace(require_string(
        *registered, "stop_command_support.registered_command_kind_id"));
  }
  return {
      kind,
      std::move(registered_id),
      decode_role_ids<core::EndpointRoleId>(
          require_member(value, "command_endpoint_role_ids"),
          "stop_command_support.command_endpoint_role_ids"),
  };
}

[[nodiscard]] core::ProviderStopEvidenceSupportDTO decode_evidence_support(
    const CanonicalValue& value) {
  const auto kind = parse_evidence_kind(require_string(
      require_member(value, "evidence_kind"),
      "stop_evidence_support.evidence_kind"));
  const auto* registered =
      value.find_object_member("registered_evidence_kind_id");
  const bool requires_registered =
      kind == core::StopEvidenceRequirementKind::kRegistered;
  if ((registered != nullptr) != requires_registered) {
    fail("provider-manifest Stop evidence has invalid registered-kind "
         "presence");
  }

  std::optional<core::RegisteredStopEvidenceKindId> registered_id;
  if (registered != nullptr) {
    registered_id.emplace(require_string(
        *registered, "stop_evidence_support.registered_evidence_kind_id"));
  }
  return {
      kind,
      std::move(registered_id),
      decode_role_ids<core::EndpointRoleId>(
          require_member(value, "evidence_endpoint_role_ids"),
          "stop_evidence_support.evidence_endpoint_role_ids"),
  };
}

[[nodiscard]] core::ProviderManifestDTO decode_normalized(
    const CanonicalValue& normalized) {
  std::vector<core::ProviderCapabilityOfferDTO> capabilities;
  const auto& capability_rows =
      require_array(require_member(normalized, "offered_capabilities"),
                    "offered_capabilities");
  capabilities.reserve(capability_rows.size());
  for (const auto& row : capability_rows) {
    capabilities.push_back({
        core::CapabilityId(require_string(
            require_member(row, "capability_id"),
            "offered_capabilities.capability_id")),
        core::CapabilityDefinitionRevision(require_string(
            require_member(row, "capability_definition_revision"),
            "offered_capabilities.capability_definition_revision")),
        decode_role_ids<core::EndpointRoleId>(
            require_member(row, "required_endpoint_role_ids"),
            "offered_capabilities.required_endpoint_role_ids"),
    });
  }

  std::vector<core::ProviderContractDeclarationDTO> contracts;
  const auto& contract_rows =
      require_array(require_member(normalized, "provider_contracts"),
                    "provider_contracts");
  contracts.reserve(contract_rows.size());
  for (const auto& row : contract_rows) {
    const auto* command = row.find_object_member("stop_command_support");
    const auto* evidence = row.find_object_member("stop_evidence_support");
    if (command == nullptr && evidence == nullptr) {
      fail("provider-manifest contract declares neither command nor evidence "
           "support");
    }
    contracts.push_back({
        decode_provider_contract_ref(
            require_member(row, "provider_contract_ref")),
        command == nullptr
            ? std::nullopt
            : std::optional<core::ProviderStopCommandSupportDTO>(
                  decode_command_support(*command)),
        evidence == nullptr
            ? std::nullopt
            : std::optional<core::ProviderStopEvidenceSupportDTO>(
                  decode_evidence_support(*evidence)),
    });
  }

  return {
      core::ProviderManifestId(require_string(
          require_member(normalized, "provider_manifest_id"),
          "provider_manifest_id")),
      core::ProviderManifestRevision(require_string(
          require_member(normalized, "provider_manifest_revision"),
          "provider_manifest_revision")),
      std::move(capabilities),
      std::move(contracts),
  };
}

void validate_capability_references(
    const core::ProviderManifestDTO& manifest) {
  for (const auto& offer : manifest.offered_capabilities) {
    const auto* definition =
        core::find_builtin_capability_definition(offer.capability_id.value());
    if (definition == nullptr ||
        offer.capability_definition_revision.value() !=
            definition->capability_definition_revision) {
      fail("provider-manifest selects an unqualified capability definition");
    }
  }
}

void validate_provider_contract_schemas(
    const core::ProviderManifestDTO& manifest) {
  const auto& registry = build_qualified_closed_schema_registry();
  for (const auto& contract : manifest.provider_contracts) {
    const auto& reference = contract.provider_contract_ref;
    const auto* selected = registry.find_qualified_value_schema(
        reference.contract_schema_id, reference.contract_schema_revision);
    if (selected == nullptr) {
      fail("provider-manifest selects an unqualified provider-contract value "
           "schema");
    }
    const auto root_shape = selected->compiled_schema().root_shape();
    if (root_shape != ClosedSchemaRootShape::kObject &&
        root_shape != ClosedSchemaRootShape::kTaggedUnion) {
      fail("provider-contract value schema must have an object or tagged-union "
           "root");
    }
  }
}

void validate_endpoint_role_closure(
    const core::ProviderManifestDTO& manifest) {
  std::set<std::string> distinct_roles;
  const auto add_roles = [&distinct_roles](
                             const std::vector<core::EndpointRoleId>& roles) {
    for (const auto& role : roles) {
      distinct_roles.insert(role.value());
      if (distinct_roles.size() >
          core::kMaxProviderManifestEndpointRoles) {
        fail("provider-manifest distinct endpoint-role union exceeds its "
             "fixed bound");
      }
    }
  };

  for (const auto& offer : manifest.offered_capabilities) {
    add_roles(offer.required_endpoint_role_ids);
  }
  for (const auto& contract : manifest.provider_contracts) {
    if (contract.stop_command_support.has_value()) {
      add_roles(contract.stop_command_support->command_endpoint_role_ids);
    }
    if (contract.stop_evidence_support.has_value()) {
      add_roles(contract.stop_evidence_support->evidence_endpoint_role_ids);
    }
  }
}

}  // namespace

QualifiedProviderManifest resolve_and_qualify_provider_manifest(
    const core::ProviderManifestCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kProviderManifestApiVersion, "provider manifest");
        } catch (const detail::ExactCatalogReferentError& error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("provider_manifest"),
      std::string(core::kProviderManifestApiVersion),
      "provider_manifest_revision"};
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("provider_manifest_revision does not match normalized semantic "
         "content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  validate_capability_references(decoded);
  validate_provider_contract_schemas(decoded);
  validate_endpoint_role_closure(decoded);
  if (decoded.provider_manifest_id != reference.record_id ||
      decoded.provider_manifest_revision != reference.record_revision) {
    fail("provider-manifest typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedProviderManifest(reference, std::move(decoded),
                                   normalized.normalized_root());
}

}  // namespace manyforge::deployment
