#include "manyforge_deployment/runtime_contract_registry.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_deployment/runtime_contract_capability_declarations.hpp>

#include "native_execution_runtime_unit_internal.hpp"
#include "reference_runtime_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

using core::RuntimeCapabilityContractRefDTO;

template <std::size_t... N>
[[nodiscard]] std::vector<RuntimeCapabilityContractRefDTO> declared_capabilities(
    const std::array<std::string_view, N>&... groups) {
  std::vector<RuntimeCapabilityContractRefDTO> result;
  result.reserve((N + ...));
  const auto append = [&](const auto& capability_ids) {
    for (const auto capability_id : capability_ids) {
      const auto* definition = core::find_builtin_capability_definition(capability_id);
      if (definition == nullptr) {
        throw std::logic_error(
            "runtime contract declaration names unknown "
            "capability " +
            std::string(capability_id));
      }
      result.emplace_back(core::CapabilityId(std::string(capability_id)),
                          core::CapabilityDefinitionRevision(
                              std::string(definition->capability_definition_revision)));
    }
  };
  (append(groups), ...);
  return result;
}

[[nodiscard]] std::string compatibility_diagnostic(
    const core::ErrorDTO& error) {
  std::string result = error.message;
  for (const auto& [key, value] : error.details) {
    result += " (" + key + "=" + value + ")";
  }
  return result;
}

[[nodiscard]] core::RuntimeContractOfferDTO make_offer(
    std::string_view family, std::vector<core::ApiContractId> api_ids,
    std::vector<RuntimeCapabilityContractRefDTO> capabilities = {}) {
  std::sort(api_ids.begin(), api_ids.end());
  std::sort(capabilities.begin(), capabilities.end(),
            [](const auto& left, const auto& right) {
              if (left.capability_id != right.capability_id) {
                return left.capability_id < right.capability_id;
              }
              return left.capability_definition_revision <
                     right.capability_definition_revision;
            });
  const auto result = core::MakeRuntimeContractOffer(
      core::RuntimeImplementationFamilyId(std::string(family)),
      std::move(api_ids), std::move(capabilities));
  if (!result.ok()) {
    throw std::logic_error("maintained runtime contract offer is invalid: " +
                           result.error().message);
  }
  return result.value();
}

[[nodiscard]] RuntimeContractRoleApiMapping role_mapping(
    std::string_view role, std::string_view api, std::string_view owner) {
  return {core::RuntimeRoleId(std::string(role)),
          core::ApiContractId(std::string(api)), std::string(owner)};
}

[[nodiscard]] RuntimeContractEndpointApiMapping endpoint_mapping(
    std::string_view role, std::string_view api, std::string_view owner) {
  return {core::EndpointRoleId(std::string(role)),
          core::ApiContractId(std::string(api)), std::string(owner)};
}

void validate_row(const RuntimeContractDeclaration& row) {
  const auto exact_product = [&]() {
    if (row.runtime_profile_id().value() ==
        detail::kReferenceRuntimeProfileId) {
      return row.runtime_product_id().value() ==
                 detail::kReferenceRuntimeProductContractId &&
             row.runtime_product_revision() ==
                 detail::reference_runtime_product_contract_revision();
    }
    if (row.runtime_profile_id().value() ==
        detail::kRos2ControlExecutionRuntimeProfileId) {
      return row.runtime_product_id().value() ==
                 detail::kRos2ControlExecutionRuntimeProductContractId &&
             row.runtime_product_revision() ==
                 detail::ros2_control_execution_runtime_product_contract_revision();
    }
    return false;
  }();
  if (!exact_product) {
    throw std::logic_error(
        "runtime contract row has no exact qualified runtime product");
  }
  const auto exact_profile = [&]() {
    if (row.runtime_profile_id().value() ==
        detail::kReferenceRuntimeProfileId) {
      return row.runtime_profile_revision().value() ==
                 detail::reference_runtime_profile_revision() &&
             row.offer().runtime_implementation_id.value() ==
                 detail::kReferenceRuntimeImplementationId;
    }
    if (row.runtime_profile_id().value() ==
        detail::kRos2ControlExecutionRuntimeProfileId) {
      return row.runtime_profile_revision().value() ==
                 detail::ros2_control_execution_runtime_profile_revision() &&
             row.offer().runtime_implementation_id.value() ==
                 detail::kRos2ControlExecutionRuntimeImplementationId;
    }
    return false;
  }();
  if (!exact_profile) {
    throw std::logic_error(
        "runtime contract row has no exact qualified profile/family pair");
  }
  const auto verified = core::VerifyRuntimeContractOffer(row.offer());
  if (!verified.ok()) {
    throw std::logic_error("runtime contract row offer failed verification: " +
                           verified.error().message);
  }
  std::set<std::string, std::less<>> mapped_api_ids;
  std::map<std::string, std::string, std::less<>> owners;
  std::set<std::string, std::less<>> runtime_roles;
  for (const auto& mapping : row.runtime_role_api_mappings()) {
    if (mapping.semantic_owner_id.empty() ||
        !runtime_roles.insert(mapping.runtime_role_id.value()).second ||
        !mapped_api_ids.insert(mapping.api_contract_id.value()).second) {
      throw std::logic_error(
          "runtime contract row has duplicate runtime-role/API ownership");
    }
    const auto [owner, inserted] = owners.emplace(
        mapping.api_contract_id.value(), mapping.semantic_owner_id);
    if (!inserted && owner->second != mapping.semantic_owner_id) {
      throw std::logic_error(
          "runtime API contract ID has conflicting semantic ownership");
    }
  }
  std::set<std::string, std::less<>> endpoint_roles;
  for (const auto& mapping : row.source_endpoint_api_mappings()) {
    if (mapping.semantic_owner_id.empty() ||
        !endpoint_roles.insert(mapping.source_endpoint_role_id.value())
             .second ||
        !mapped_api_ids.insert(mapping.api_contract_id.value()).second) {
      throw std::logic_error(
          "runtime contract row has duplicate source-endpoint/API ownership");
    }
    const auto [owner, inserted] = owners.emplace(
        mapping.api_contract_id.value(), mapping.semantic_owner_id);
    if (!inserted && owner->second != mapping.semantic_owner_id) {
      throw std::logic_error(
          "runtime API contract ID has conflicting semantic ownership");
    }
  }
  if (mapped_api_ids.size() != row.offer().provided_api_contract_ids.size() ||
      !std::equal(mapped_api_ids.begin(), mapped_api_ids.end(),
                  row.offer().provided_api_contract_ids.begin(),
                  [](const auto& left, const auto& right) {
                    return left == right.value();
                  })) {
    throw std::logic_error(
        "runtime contract API offer differs from its complete role mapping");
  }
  if (endpoint_roles.find(
          row.runtime_profile_id().value() ==
                  detail::kReferenceRuntimeProfileId
              ? std::string(detail::kReferenceRuntimeStatusEndpointRoleId)
              : std::string(
                          detail::
                              kRos2ControlExecutionRuntimeStatusEndpointRoleId)) ==
      endpoint_roles.end()) {
    throw std::logic_error(
        "runtime contract row omits its mandatory supervision/status mapping");
  }
  std::set<std::string, std::less<>> expected_runtime_roles;
  std::set<std::string, std::less<>> expected_endpoint_roles;
  if (row.runtime_profile_id().value() == detail::kReferenceRuntimeProfileId) {
    expected_runtime_roles.insert(
        std::string(detail::kReferenceRuntimeRoleId));
    expected_endpoint_roles.insert(
        std::string(detail::kReferenceRuntimeStatusEndpointRoleId));
  } else if (row.runtime_profile_id().value() ==
             detail::kRos2ControlExecutionRuntimeProfileId) {
    expected_runtime_roles.insert(
        std::string(detail::kRos2ControlExecutionRuntimeRoleId));
    expected_endpoint_roles = {
        std::string(
            detail::kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId),
        std::string(
            detail::kRos2ControlExecutionRuntimeProjectionEndpointRoleId),
        std::string(detail::kRos2ControlExecutionRuntimeStatusEndpointRoleId)};
  } else {
    throw std::logic_error("runtime contract row names an unknown profile");
  }
  if (runtime_roles != expected_runtime_roles ||
      endpoint_roles != expected_endpoint_roles) {
    throw std::logic_error(
        "runtime contract row has incomplete or uncovered profile-role "
        "mappings");
  }
  std::set<std::string, std::less<>> capabilities;
  for (const auto& ref : row.offer().provided_capabilities) {
    const auto* descriptor =
        core::find_builtin_capability_definition(ref.capability_id.value());
    if (descriptor == nullptr ||
        descriptor->capability_definition_revision !=
            ref.capability_definition_revision.value() ||
        !capabilities.insert(ref.capability_id.value()).second) {
      throw std::logic_error(
          "runtime contract row has an unknown, stale, or duplicate "
          "capability reference");
    }
  }
}

}  // namespace

std::vector<RuntimeContractDeclaration>
RuntimeContractDeclarationRegistry::make_rows() {
  const std::string reference_product_revision =
      detail::reference_runtime_product_contract_revision().value();
  const std::string ros_product_revision =
      detail::ros2_control_execution_runtime_product_contract_revision().value();
  std::vector<RuntimeContractDeclaration> rows;
  rows.push_back(RuntimeContractDeclaration(
      core::RuntimeProfileId(std::string(detail::kReferenceRuntimeProfileId)),
      core::RuntimeProfileRevision(
          detail::reference_runtime_profile_revision()),
      core::ProductContractId(
          std::string(detail::kReferenceRuntimeProductContractId)),
      core::ProductContractRevision(reference_product_revision),
      make_offer(
          detail::kReferenceRuntimeImplementationId,
          {core::ApiContractId(
               std::string(detail::kReferenceRuntimeRoleApiContractId)),
           core::ApiContractId(
               std::string(detail::kReferenceRuntimeStatusApiContractId))},
          {}),
      {role_mapping(detail::kReferenceRuntimeRoleId,
                    detail::kReferenceRuntimeRoleApiContractId,
                    "reference-runtime"),
       },
      {endpoint_mapping(detail::kReferenceRuntimeStatusEndpointRoleId,
                        detail::kReferenceRuntimeStatusApiContractId,
                        "reference-runtime")}));
  rows.push_back(RuntimeContractDeclaration(
      core::RuntimeProfileId(
          std::string(detail::kRos2ControlExecutionRuntimeProfileId)),
      core::RuntimeProfileRevision(
          detail::ros2_control_execution_runtime_profile_revision()),
      core::ProductContractId(std::string(
          detail::kRos2ControlExecutionRuntimeProductContractId)),
      core::ProductContractRevision(ros_product_revision),
      make_offer(
          detail::kRos2ControlExecutionRuntimeImplementationId,
          {core::ApiContractId(std::string(
               ros2_control_execution_runtime_contract::
                   kRuntimeRoleApiContractId)),
           core::ApiContractId(std::string(
               ros2_control_execution_runtime_contract::
                   kStatusEndpointApiContractId)),
           core::ApiContractId(std::string(
               ros2_control_execution_runtime_contract::
                   kProjectionEndpointApiContractId)),
           core::ApiContractId(std::string(
               ros2_control_execution_runtime_contract::
                   kLogicalSceneEndpointApiContractId))},
          declared_capabilities(
              runtime_contract_capabilities::kComponentLifecycleServices,
              runtime_contract_capabilities::kExecutionServices,
              runtime_contract_capabilities::kLogicalSceneServices)),
      {role_mapping(
          detail::kRos2ControlExecutionRuntimeRoleId,
          ros2_control_execution_runtime_contract::kRuntimeRoleApiContractId,
          "ros2-control-execution-host")},
      {endpoint_mapping(
           detail::kRos2ControlExecutionRuntimeStatusEndpointRoleId,
           ros2_control_execution_runtime_contract::
               kStatusEndpointApiContractId,
           "ros2-control-execution-host"),
       endpoint_mapping(
           detail::kRos2ControlExecutionRuntimeProjectionEndpointRoleId,
           ros2_control_execution_runtime_contract::
               kProjectionEndpointApiContractId,
           "ros2-control-execution-host"),
       endpoint_mapping(
           detail::kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId,
           ros2_control_execution_runtime_contract::
               kLogicalSceneEndpointApiContractId,
           "ros2-control-execution-host")}));
  return rows;
}

namespace {

[[nodiscard]] core::RuntimeContractManifestSetDTO make_manifest_set(
    const std::vector<RuntimeContractDeclaration>& rows) {
  std::vector<core::RuntimeContractOfferDTO> offers;
  offers.reserve(rows.size());
  for (const auto& row : rows) {
    offers.push_back(row.offer());
  }
  const auto result = core::MakeRuntimeContractManifestSet(std::move(offers));
  if (!result.ok()) {
    throw std::logic_error("maintained runtime manifest set is invalid: " +
                           result.error().message);
  }
  return result.value();
}

}  // namespace

RuntimeContractDeclaration::RuntimeContractDeclaration(
    core::RuntimeProfileId runtime_profile_id,
    core::RuntimeProfileRevision runtime_profile_revision,
    core::ProductContractId runtime_product_id,
    core::ProductContractRevision runtime_product_revision,
    core::RuntimeContractOfferDTO offer,
    std::vector<RuntimeContractRoleApiMapping> runtime_role_api_mappings,
    std::vector<RuntimeContractEndpointApiMapping>
        source_endpoint_api_mappings)
    : runtime_profile_id_(std::move(runtime_profile_id)),
      runtime_profile_revision_(std::move(runtime_profile_revision)),
      runtime_product_id_(std::move(runtime_product_id)),
      runtime_product_revision_(std::move(runtime_product_revision)),
      offer_(std::move(offer)),
      runtime_role_api_mappings_(std::move(runtime_role_api_mappings)),
      source_endpoint_api_mappings_(
          std::move(source_endpoint_api_mappings)) {}

RuntimeContractDeclarationRegistry::RuntimeContractDeclarationRegistry()
    : contracts_(make_rows()),
      package_manifest_set_(make_manifest_set(contracts_)) {
  std::set<std::string, std::less<>> families;
  std::map<std::string, std::string, std::less<>> api_owners;
  for (const auto& row : contracts_) {
    validate_row(row);
    if (!families.insert(row.offer().runtime_implementation_id.value())
             .second) {
      throw std::logic_error(
          "runtime contract registry repeats an implementation family");
    }
    auto register_owner = [&api_owners](const auto& mapping) {
      if (!api_owners
               .emplace(mapping.api_contract_id.value(),
                        mapping.semantic_owner_id)
               .second) {
        throw std::logic_error(
            "runtime contract registry repeats an API contract ownership");
      }
    };
    for (const auto& mapping : row.runtime_role_api_mappings()) {
      register_owner(mapping);
    }
    for (const auto& mapping : row.source_endpoint_api_mappings()) {
      register_owner(mapping);
    }
  }
  if (contracts_.empty() ||
      package_manifest_set_.offers.size() != contracts_.size()) {
    throw std::logic_error("runtime contract registry has uncovered rows");
  }
}

const RuntimeContractDeclaration*
RuntimeContractDeclarationRegistry::find(
    const core::RuntimeProfileId& runtime_profile_id,
    const core::RuntimeProfileRevision& runtime_profile_revision,
    const core::ProductContractId& runtime_product_id,
    const core::ProductContractRevision& runtime_product_revision) const
    noexcept {
  const auto it = std::find_if(
      contracts_.begin(), contracts_.end(), [&](const auto& row) {
        return row.runtime_profile_id() == runtime_profile_id &&
               row.runtime_profile_revision() == runtime_profile_revision &&
               row.runtime_product_id() == runtime_product_id &&
               row.runtime_product_revision() == runtime_product_revision;
      });
  return it == contracts_.end() ? nullptr : &*it;
}

const RuntimeContractDeclarationRegistry&
runtime_contract_declaration_registry() {
  static const RuntimeContractDeclarationRegistry registry;
  return registry;
}

namespace detail {

CanonicalValue runtime_contract_offer_projection(
    const core::RuntimeContractOfferDTO& offer) {
  const auto encoded = core::EncodeRuntimeContractOffer(offer);
  if (!encoded.ok()) {
    throw std::logic_error("runtime contract offer encoding failed: " +
                           encoded.error().message);
  }
  return parse_strict_json_model_yaml(encoded.value()).root();
}

core::RuntimeContractOfferDTO decode_runtime_contract_offer_projection(
    const CanonicalValue& value, std::string_view field) {
  try {
    const auto decoded =
        core::DecodeRuntimeContractOffer(canonical_json(value));
    if (!decoded.ok()) {
      fail_retained_bundle(std::string(field) + " is not a verified offer");
    }
    return decoded.value();
  } catch (const std::exception& error) {
    fail_retained_bundle(std::string(field) + " is invalid: " + error.what());
  }
}

const RuntimeContractDeclaration& require_runtime_contract(
    const core::RuntimeProfileId& runtime_profile_id,
    const core::RuntimeProfileRevision& runtime_profile_revision,
    const core::ProductContractId& runtime_product_id,
    const core::ProductContractRevision& runtime_product_revision) {
  const auto* result = runtime_contract_declaration_registry().find(
      runtime_profile_id, runtime_profile_revision, runtime_product_id,
      runtime_product_revision);
  if (result == nullptr) {
    if (runtime_profile_id.value() == detail::kRos2ControlExecutionRuntimeProfileId) {
      fail_retained_bundle("ROS runtime profile/product is not qualified by this build; "
                           "recompile the authored Deployment with the current ManyForge version");
    }
    fail_retained_bundle(
        "runtime profile/product has no component contract declaration");
  }
  return *result;
}

const RuntimeContractDeclaration& require_runtime_contract_for_unit(
    const core::RuntimeUnitDTO& runtime_unit) {
  if (runtime_unit.runtime_profile_id.value() ==
      detail::kReferenceRuntimeProfileId) {
    return require_runtime_contract(
        runtime_unit.runtime_profile_id, runtime_unit.runtime_profile_revision,
        core::ProductContractId(
            std::string(detail::kReferenceRuntimeProductContractId)),
        detail::reference_runtime_product_contract_revision());
  }
  if (runtime_unit.runtime_profile_id.value() ==
      detail::kRos2ControlExecutionRuntimeProfileId) {
    return require_runtime_contract(
        runtime_unit.runtime_profile_id, runtime_unit.runtime_profile_revision,
        core::ProductContractId(std::string(
            detail::kRos2ControlExecutionRuntimeProductContractId)),
        detail::ros2_control_execution_runtime_product_contract_revision());
  }
  fail_retained_bundle(
      "runtime unit selects no maintained runtime contract family");
}

namespace {

[[nodiscard]] bool has_capability(
    std::span<const RuntimeCapabilityContractRefDTO> refs,
    const RuntimeCapabilityContractRefDTO& candidate) {
  return std::any_of(refs.begin(), refs.end(), [&](const auto& ref) {
    return ref == candidate;
  });
}

[[nodiscard]] std::string mapping_api(
    const RuntimeContractDeclaration& row,
    const core::EndpointRoleId& endpoint_role_id) {
  const auto mapping = std::find_if(
      row.source_endpoint_api_mappings().begin(),
      row.source_endpoint_api_mappings().end(), [&](const auto& candidate) {
        return candidate.source_endpoint_role_id == endpoint_role_id;
      });
  if (mapping == row.source_endpoint_api_mappings().end()) {
    fail_retained_bundle(
        "runtime requirement uses an unmapped qualified source endpoint role");
  }
  return mapping->api_contract_id.value();
}

}  // namespace

std::vector<core::RetainedRuntimeContractRequirementDTO>
derive_runtime_contract_requirements(
    const core::CellGraphDTO& graph,
    std::span<const core::CapabilityProviderBindingDTO>
        capability_provider_bindings,
    std::span<const LogicalCapabilityTemplate> capabilities,
    std::span<const LogicalProviderEndpointTemplate> provider_endpoints,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const core::RetainedRuntimeUnitProductsDTO>
        runtime_unit_products) {
  std::vector<core::RetainedRuntimeContractRequirementDTO> result;
  result.reserve(graph.runtime_units.size());
  for (const auto& unit : graph.runtime_units) {
    const auto& row = require_runtime_contract_for_unit(unit);
    const auto profile = std::find_if(
        runtime_profiles.begin(), runtime_profiles.end(),
        [&](const auto& candidate) {
          return candidate.value().runtime_profile_id ==
                     unit.runtime_profile_id &&
                 candidate.value().runtime_profile_revision ==
                     unit.runtime_profile_revision;
        });
    if (profile == runtime_profiles.end()) {
      fail_retained_bundle(
          "runtime requirement has no exact qualified runtime profile");
    }

    std::set<std::string, std::less<>> api_ids;
    for (const auto& mapping : row.runtime_role_api_mappings()) {
      api_ids.insert(mapping.api_contract_id.value());
    }
    std::set<std::string, std::less<>> source_runtime_endpoints;
    for (const auto& endpoint : provider_endpoints) {
      const auto* source =
          std::get_if<RuntimeEndpointSourceTemplate>(&endpoint.source);
      if (source == nullptr || source->runtime_unit_id != unit.runtime_unit_id) {
        continue;
      }
      if (source->runtime_profile_id != unit.runtime_profile_id ||
          source->runtime_profile_revision != unit.runtime_profile_revision) {
        fail_retained_bundle(
            "runtime requirement source endpoint has a stale profile "
            "qualification");
      }
      source_runtime_endpoints.insert(source->source_endpoint_role_id.value());
    }
    for (const auto& binding : capability_provider_bindings) {
      for (const auto& endpoint : binding.endpoint_bindings) {
        if (endpoint.source_kind !=
                core::CapabilityEndpointSourceKind::kRuntimeUnit ||
            !std::holds_alternative<core::RuntimeUnitId>(
                endpoint.source_id) ||
            std::get<core::RuntimeUnitId>(endpoint.source_id) !=
                unit.runtime_unit_id) {
          continue;
        }
        source_runtime_endpoints.insert(
            endpoint.source_endpoint_role_id.value());
      }
    }
    // Supervision is mandatory even for a runtime with no provider binding.
    source_runtime_endpoints.insert(
        profile->value().supervision_contract.status_endpoint_role_id.value());
    for (const auto& endpoint_role : source_runtime_endpoints) {
      api_ids.insert(mapping_api(
          row, core::EndpointRoleId(endpoint_role)));
    }

    std::vector<RuntimeCapabilityContractRefDTO> required_capabilities;
    for (const auto& capability : capabilities) {
      const auto add = [&](const core::CapabilityId& id,
                           const core::CapabilityDefinitionRevision& revision) {
        const RuntimeCapabilityContractRefDTO ref{id, revision};
        if (!has_capability(required_capabilities, ref)) {
          required_capabilities.push_back(ref);
        }
      };
      if (const auto* control_owner =
              std::get_if<ControlBindingCapabilityOwner>(&capability.owner);
          control_owner != nullptr) {
        const auto binding = std::find_if(
            graph.control_bindings.begin(), graph.control_bindings.end(),
            [&](const auto& candidate) {
              return candidate.control_binding_id ==
                         control_owner->control_binding_id &&
                     candidate.control_binding_revision ==
                         control_owner->control_binding_revision;
            });
        if (binding == graph.control_bindings.end()) {
          fail_retained_bundle(
              "runtime requirement capability names an unknown control "
              "binding");
        }
        if (binding->capability_id != capability.capability_id ||
            binding->capability_definition_revision !=
                capability.capability_definition_revision) {
          fail_retained_bundle(
              "runtime requirement capability/control binding identity "
              "differs from the exact qualified capability");
        }
        if (binding->provider_runtime_unit_id == unit.runtime_unit_id) {
          add(capability.capability_id,
              capability.capability_definition_revision);
        }
        continue;
      }
      const auto* provider_owner =
          std::get_if<CapabilityProviderCapabilityOwner>(&capability.owner);
      if (provider_owner == nullptr) {
        fail_retained_bundle(
            "runtime requirement capability has an unsupported owner");
      }
      const auto binding = std::find_if(
          capability_provider_bindings.begin(), capability_provider_bindings.end(),
          [&](const auto& candidate) {
            return candidate.capability_provider_binding_id ==
                       provider_owner->capability_provider_binding_id &&
                   candidate.capability_provider_binding_revision ==
                       provider_owner->capability_provider_binding_revision;
          });
      if (binding == capability_provider_bindings.end()) {
        fail_retained_bundle(
            "runtime requirement capability names an unknown provider binding");
      }
      if (binding->capability_id != capability.capability_id ||
          binding->capability_definition_revision !=
              capability.capability_definition_revision) {
        fail_retained_bundle(
            "runtime requirement capability/provider binding identity "
            "differs from the exact qualified capability");
      }
      std::set<std::string, std::less<>> binding_runtime_units;
      for (const auto& endpoint : provider_endpoints) {
        if (endpoint.capability_provider_binding_id !=
                provider_owner->capability_provider_binding_id ||
            endpoint.capability_provider_binding_revision !=
                provider_owner->capability_provider_binding_revision) {
          continue;
        }
        if (const auto* source =
                std::get_if<RuntimeEndpointSourceTemplate>(&endpoint.source);
            source != nullptr) {
          binding_runtime_units.insert(source->runtime_unit_id.value());
        }
      }
      if (binding_runtime_units.size() > 1U) {
        fail_retained_bundle(
            "provider capability binding spans multiple runtime units");
      }
      if (binding_runtime_units.contains(unit.runtime_unit_id.value())) {
        add(capability.capability_id,
            capability.capability_definition_revision);
      }
    }
    if (capabilities.empty()) {
      for (const auto& binding : graph.control_bindings) {
        if (binding.provider_runtime_unit_id == unit.runtime_unit_id) {
          required_capabilities.push_back(
              {binding.capability_id,
               binding.capability_definition_revision});
        }
      }
      for (const auto& binding : capability_provider_bindings) {
        std::set<std::string, std::less<>> binding_runtime_units;
        for (const auto& endpoint : binding.endpoint_bindings) {
          if (endpoint.source_kind ==
                  core::CapabilityEndpointSourceKind::kRuntimeUnit &&
              std::holds_alternative<core::RuntimeUnitId>(
                  endpoint.source_id)) {
            binding_runtime_units.insert(
                std::get<core::RuntimeUnitId>(endpoint.source_id).value());
          }
        }
        if (binding_runtime_units.size() > 1U) {
          fail_retained_bundle(
              "provider capability binding spans multiple runtime units");
        }
        if (binding_runtime_units.contains(unit.runtime_unit_id.value())) {
          required_capabilities.push_back(
              {binding.capability_id,
               binding.capability_definition_revision});
        }
      }
      std::sort(required_capabilities.begin(), required_capabilities.end(),
                [](const auto& left, const auto& right) {
                  if (left.capability_id != right.capability_id) {
                    return left.capability_id < right.capability_id;
                  }
                  return left.capability_definition_revision <
                         right.capability_definition_revision;
                });
      required_capabilities.erase(
          std::unique(required_capabilities.begin(),
                      required_capabilities.end()),
          required_capabilities.end());
    }
    std::sort(required_capabilities.begin(), required_capabilities.end(),
              [](const auto& left, const auto& right) {
                if (left.capability_id != right.capability_id) {
                  return left.capability_id < right.capability_id;
                }
                return left.capability_definition_revision <
                       right.capability_definition_revision;
              });
    std::vector<core::ApiContractId> required_api_ids;
    required_api_ids.reserve(api_ids.size());
    for (const auto& id : api_ids) {
      required_api_ids.emplace_back(id);
    }
    const auto requirement = core::MakeRuntimeContractRequirement(
        row.offer().runtime_implementation_id, std::move(required_api_ids),
        std::move(required_capabilities));
    if (!requirement.ok()) {
      fail_retained_bundle(
          "runtime requirement could not be canonicalized");
    }
    const auto compatible = core::CheckRuntimeContractCompatibility(
        row.offer(), requirement.value());
    if (!compatible.ok()) {
      fail_retained_bundle(
          "runtime requirement is incompatible with its selected offer: " +
          compatibility_diagnostic(compatible.error()));
    }
    result.push_back({unit.runtime_unit_id, requirement.value()});
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) {
              return left.runtime_unit_id < right.runtime_unit_id;
            });
  require_runtime_contract_requirements(
      graph, capability_provider_bindings, result, runtime_unit_products);
  return result;
}

void require_runtime_contract_requirements(
    const core::CellGraphDTO& graph,
    std::span<const core::CapabilityProviderBindingDTO>
        capability_provider_bindings,
    std::span<const core::RetainedRuntimeContractRequirementDTO> requirements,
    std::span<const core::RetainedRuntimeUnitProductsDTO>
        runtime_unit_products) {
  const auto& registry = runtime_contract_declaration_registry();
  const auto package_verified =
      core::VerifyRuntimeContractManifestSet(registry.package_manifest_set());
  if (!package_verified.ok()) {
    fail_retained_bundle(
        "maintained runtime package manifest set is not verified");
  }
  if (requirements.size() != graph.runtime_units.size() ||
      !std::is_sorted(
          requirements.begin(), requirements.end(),
          [](const auto& left, const auto& right) {
            return left.runtime_unit_id < right.runtime_unit_id;
          })) {
    fail_retained_bundle(
        "retained runtime-contract requirements do not have exact coverage "
        "or order");
  }
  std::set<std::string, std::less<>> runtime_ids;
  std::vector<std::string> graph_runtime_ids;
  graph_runtime_ids.reserve(graph.runtime_units.size());
  for (const auto& unit : graph.runtime_units) {
    graph_runtime_ids.push_back(unit.runtime_unit_id.value());
  }
  std::sort(graph_runtime_ids.begin(), graph_runtime_ids.end());
  for (std::size_t index = 0U; index < requirements.size(); ++index) {
    const auto& requirement = requirements[index];
    if (!runtime_ids.insert(requirement.runtime_unit_id.value()).second) {
      fail_retained_bundle(
          "retained runtime-contract requirements contain a duplicate");
    }
    if (index >= graph_runtime_ids.size() ||
        requirement.runtime_unit_id.value() != graph_runtime_ids[index]) {
      fail_retained_bundle(
          "retained runtime-contract requirements do not match the graph "
          "runtime set");
    }
    const auto unit = std::find_if(
        graph.runtime_units.begin(), graph.runtime_units.end(),
        [&](const auto& candidate) {
          return candidate.runtime_unit_id == requirement.runtime_unit_id;
        });
    if (unit == graph.runtime_units.end()) {
      fail_retained_bundle(
          "retained runtime-contract requirement names an extra runtime");
    }
    const auto& row = require_runtime_contract_for_unit(*unit);
    const auto package_offer = std::find_if(
        registry.package_manifest_set().offers.begin(),
        registry.package_manifest_set().offers.end(),
        [&](const auto& offer) {
          return offer.runtime_implementation_id ==
                 row.offer().runtime_implementation_id;
        });
    if (package_offer == registry.package_manifest_set().offers.end() ||
        *package_offer != row.offer()) {
      fail_retained_bundle(
          "runtime requirement offer differs from the package manifest set");
    }
    const auto package_bytes = core::EncodeRuntimeContractOffer(*package_offer);
    if (!package_bytes.ok()) {
      fail_retained_bundle("package runtime offer cannot be encoded");
    }
    const auto decoded_package_offer =
        core::DecodeRuntimeContractOffer(package_bytes.value());
    if (!decoded_package_offer.ok() ||
        decoded_package_offer.value() != row.offer()) {
      fail_retained_bundle(
          "decoded package runtime offer differs from the registry offer");
    }
    const auto verified =
        core::VerifyRuntimeContractRequirement(requirement.requirement);
    if (!verified.ok()) {
      fail_retained_bundle(
          "retained runtime-contract requirement is not canonical");
    }
    const auto compatible = core::CheckRuntimeContractCompatibility(
        row.offer(), requirement.requirement);
    if (!compatible.ok()) {
      fail_retained_bundle("retained runtime-contract requirement is "
                           "incompatible: " +
                           compatibility_diagnostic(compatible.error()));
    }
  }
  if (!runtime_unit_products.empty()) {
    if (runtime_unit_products.size() != graph.runtime_units.size()) {
      fail_retained_bundle(
          "runtime products do not have exact runtime requirement coverage");
    }
    std::vector<std::string> product_runtime_ids;
    product_runtime_ids.reserve(runtime_unit_products.size());
    for (const auto& group : runtime_unit_products) {
      if (!runtime_ids.contains(group.runtime_unit_id.value()) ||
          group.products.size() != 1U) {
        fail_retained_bundle(
            "runtime product group is not one-to-one with requirements");
      }
      product_runtime_ids.push_back(group.runtime_unit_id.value());
      const auto graph_unit = std::find_if(
          graph.runtime_units.begin(), graph.runtime_units.end(),
          [&](const auto& candidate) {
            return candidate.runtime_unit_id == group.runtime_unit_id;
          });
      if (graph_unit == graph.runtime_units.end()) {
        fail_retained_bundle(
            "runtime product group names an unknown runtime unit");
      }
      const auto& row = require_runtime_contract_for_unit(*graph_unit);
      const auto& product = group.products.front();
      if (product.product_contract_id != row.runtime_product_id() ||
          product.product_contract_revision !=
              row.runtime_product_revision()) {
        fail_retained_bundle(
            "runtime product does not match its selected contract row");
      }
    }
    std::sort(product_runtime_ids.begin(), product_runtime_ids.end());
    if (product_runtime_ids != graph_runtime_ids) {
      fail_retained_bundle(
          "runtime product groups do not have exact canonical coverage");
    }
  }
  (void)capability_provider_bindings;
}

}  // namespace detail
}  // namespace manyforge::deployment
