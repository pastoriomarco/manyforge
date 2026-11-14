#include <algorithm>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <string_view>

namespace manyforge::core {
namespace {

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= kMaxLifecycleIdentityBytes;
}

}  // namespace

bool ProviderControlDomainDTO::locally_valid() const noexcept {
  if (!bounded_identity(controllable_component_id) || !bounded_identity(provider_runtime_unit_id) ||
      !bounded_identity(provider_instance_id) || !bounded_identity(provider_manifest_id) ||
      !IsExactLowercaseSha256Digest(provider_manifest_revision.value()) ||
      !IsExactLowercaseSha256Digest(lifecycle_plan_catalog_digest.value()) ||
      !IsExactLowercaseSha256Digest(signal_configuration_digest.value()) ||
      lifecycle_command_timeout_ns <= 0 ||
      lifecycle_command_timeout_ns > kMaxLifecycleTransactionTimeoutNs || signal_names.empty() ||
      signal_names.size() > kMaxLifecycleCurrentResources ||
      !std::is_sorted(signal_names.begin(), signal_names.end()) ||
      std::adjacent_find(signal_names.begin(), signal_names.end()) != signal_names.end())
    return false;
  return std::all_of(signal_names.begin(), signal_names.end(), [](const auto& name) {
    return !name.empty() && name.size() <= kMaxLifecycleIdentityBytes;
  });
}

bool ResolvedLifecycleProviderOccurrenceDTO::locally_valid() const noexcept {
  return bounded_identity(control_domain_id) && bounded_identity(control_domain_revision) &&
         bounded_identity(provider_instance_id) && bounded_identity(provider_manifest_id) &&
         bounded_identity(provider_manifest_revision) &&
         bounded_identity(provider_runtime_unit_id) && bounded_identity(provider_runtime_epoch) &&
         bounded_identity(lifecycle_plan_catalog_digest);
}

bool ResolvedLifecycleControlDomainScopeDTO::locally_valid() const noexcept {
  if (!bounded_identity(control_domain_id) || !bounded_identity(control_domain_revision))
    return false;
  const auto& value = payload;
  if (!value.provider.locally_valid() || !bounded_identity(value.provider_runtime_epoch))
    return false;
  return value.output_resources.size() <= kMaxLifecycleCurrentResources &&
         std::is_sorted(value.output_resources.begin(), value.output_resources.end()) &&
         std::adjacent_find(value.output_resources.begin(), value.output_resources.end()) ==
             value.output_resources.end() &&
         std::none_of(value.output_resources.begin(), value.output_resources.end(),
                      [](const auto& id) { return !bounded_identity(id); });
}

}  // namespace manyforge::core
