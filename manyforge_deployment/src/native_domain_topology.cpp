#include <manyforge_deployment/native_domain_topology.hpp>

#include <map>
#include <set>
#include <string>

namespace manyforge::deployment {

core::Result<void>
VerifySupportedNativeDomainTopology(const core::CellGraphDTO &graph) {
  std::map<core::ControlDomainId, std::set<core::ControllableComponentId>>
      native_domains;
  for (const auto &binding : graph.control_bindings) {
    if (binding.provider_manifest_id.value() !=
        kNativeControlProviderManifestId) {
      continue;
    }
    native_domains[binding.control_domain_id].insert(
        binding.controllable_component_id);
  }
  for (const auto &[domain, components] : native_domains) {
    if (components.size() <= 1U) {
      continue;
    }
    std::string names;
    for (const auto &component : components) {
      if (!names.empty()) names += ",";
      names += component.value();
    }
    return core::Result<void>::Err(core::ErrorDTO{
        "native_shared_domain_unsupported",
        "the native provider supports one controllable component per control "
        "domain; domain " +
            domain.value() + " is shared by " + names,
        {{"reason", "native_shared_domain_unsupported"},
         {"control_domain_id", domain.value()},
         {"controllable_component_ids", names}}});
  }
  return core::Result<void>::Ok();
}

} // namespace manyforge::deployment
