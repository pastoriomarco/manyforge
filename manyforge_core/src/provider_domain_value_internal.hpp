#pragma once

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>

namespace manyforge::core::detail {

[[nodiscard]] inline content_identity::CanonicalValue ProviderDomainCounter(std::uint64_t value) {
  if (value > 9'007'199'254'740'991ULL)
    throw content_identity::CanonicalValueError(
        "provider-domain counter exceeds the I-JSON safe range");
  return content_identity::CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] inline content_identity::CanonicalValue ProviderDomainValue(
    const ProviderControlDomainDTO& value) {
  using content_identity::CanonicalValue;
  CanonicalValue::Array names;
  for (const auto& name : value.signal_names) names.emplace_back(name);
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id",
       CanonicalValue(value.controllable_component_id.value())},
      {"provider_runtime_unit_id",
       CanonicalValue(value.provider_runtime_unit_id.value())},
      {"provider_instance_id",
       CanonicalValue(value.provider_instance_id.value())},
      {"provider_manifest_id",
       CanonicalValue(value.provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(value.provider_manifest_revision.value())},
      {"lifecycle_plan_catalog_digest",
       CanonicalValue(value.lifecycle_plan_catalog_digest.value())},
      {"signal_configuration_digest",
       CanonicalValue(value.signal_configuration_digest.value())},
      {"signal_names", CanonicalValue(std::move(names))},
      {"lifecycle_command_timeout_ns",
       ProviderDomainCounter(
           static_cast<std::uint64_t>(value.lifecycle_command_timeout_ns))}});
}

[[nodiscard]] inline content_identity::CanonicalValue ProviderActiveConfigurationValue(
    const ProviderDomainActiveConfigurationDTO& value) {
  using content_identity::CanonicalValue;
  return CanonicalValue(CanonicalValue::Object{
      {"active_control_configuration_id",
       CanonicalValue(value.active_control_configuration_id.value())},
      {"active_control_configuration_generation",
       ProviderDomainCounter(value.active_control_configuration_generation.value())},
      {"source_runtime_unit_id", CanonicalValue(value.source_runtime_unit_id.value())},
      {"source_runtime_epoch", CanonicalValue(value.source_runtime_epoch.value())},
      {"source_sequence", ProviderDomainCounter(value.source_sequence)},
      {"signal_configuration_digest", CanonicalValue(value.signal_configuration_digest.value())}});
}

// Only the open response changes spelling. Snapshot identity retains its exact
// canonical preimage, independently of the public wire representation.
enum class LifecycleDomainResourceShape { kSnapshotIdentity, kWire };

[[nodiscard]] inline content_identity::CanonicalValue LifecycleDomainResourcesValue(
    const ResolvedLifecycleControlDomainScopeDTO& domain) {
  content_identity::CanonicalValue::Array resources;
  resources.reserve(domain.resource_ids().size());
  for (const auto& resource : domain.resource_ids()) resources.emplace_back(resource.value());
  return content_identity::CanonicalValue(std::move(resources));
}

[[nodiscard]] inline content_identity::CanonicalValue ResolvedProviderDomainValue(
    const ResolvedLifecycleControlDomainScopeDTO& domain, bool include_absent_configuration,
    LifecycleDomainResourceShape resource_shape = LifecycleDomainResourceShape::kSnapshotIdentity) {
  using content_identity::CanonicalValue;
  const auto& scope = *domain.provider();
  CanonicalValue::Object fields{
      {"control_domain_id", CanonicalValue(domain.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(domain.control_domain_revision.value())},
      {"kind", CanonicalValue(resource_shape == LifecycleDomainResourceShape::kWire
                                  ? "io_provider" : "provider")},
      {"provider", ProviderDomainValue(scope.provider)},
      {"provider_runtime_epoch", CanonicalValue(scope.provider_runtime_epoch.value())},
      {resource_shape == LifecycleDomainResourceShape::kWire ? "resources" : "output_resources",
       LifecycleDomainResourcesValue(domain)}};
  if (scope.active_control_configuration)
    fields.emplace_back("active_control_configuration",
                        ProviderActiveConfigurationValue(*scope.active_control_configuration));
  else if (include_absent_configuration)
    fields.emplace_back("active_control_configuration", CanonicalValue(nullptr));
  return CanonicalValue(std::move(fields));
}

}  // namespace manyforge::core::detail
