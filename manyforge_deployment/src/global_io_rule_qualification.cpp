#include <algorithm>
#include <manyforge_core/io_rule_stop_response.hpp>
#include <manyforge_deployment/global_io_rule_qualification.hpp>
#include <manyforge_deployment/program_io_stop_response_validation.hpp>
#include <map>
#include <stdexcept>

namespace manyforge::deployment {
namespace {
core::ErrorDTO invalid(const std::exception &error) {
  return {"invalid_request",
          "Global interrupt Stop entry is not qualified",
          {{"reason", error.what()}}};
}
std::vector<const core::ControlBindingDTO *>
subjects(const core::IoActivationRulePolicyBindingDTO &rule,
         const RetainedBundle &bundle) {
  std::vector<const core::ControlBindingDTO *> selected;
  if (!rule.stop_entry_stage_id)
    return selected;
  const auto *groups =
      std::get_if<core::IoActivationExecutionGroupsScopeDTO>(&rule.scope);
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    if (binding.capability_id.value() != "execution.trajectory.execute")
      continue;
    const core::IoActivationMotionSubjectDTO subject{
        binding.controllable_component_id, binding.command_target_id,
        binding.control_domain_id, binding.control_binding_id,
        binding.control_binding_revision};
    if (!groups || std::binary_search(groups->subjects.begin(),
                                      groups->subjects.end(), subject))
      selected.push_back(&binding);
  }
  if (groups && selected.size() != groups->subjects.size())
    throw std::invalid_argument("global_interrupt_group_unbound");
  if (selected.empty())
    throw std::invalid_argument("io_rule_stop_entry_target_unqualified");
  return selected;
}
core::ControlRouteId desired_route(const core::ControlBindingDTO &binding,
                                   const RetainedBundle &bundle) {
  const core::DesiredControlBindingRouteDTO *selected = nullptr;
  const auto &domains = bundle.value().cell_graph.control_domains;
  const auto domain =
      std::find_if(domains.begin(), domains.end(), [&](const auto &row) {
        return row.control_domain_id == binding.control_domain_id;
      });
  if (domain == domains.end() || domain->provider())
    throw std::invalid_argument("io_rule_stop_route_missing");
  for (const auto &configuration :
       bundle.value().desired_control_configurations)
    if (configuration.control_domain_id == domain->control_domain_id &&
        configuration.control_domain_revision ==
            domain->control_domain_revision)
      for (const auto &route : configuration.binding_routes)
        if (route.control_binding_id == binding.control_binding_id &&
            route.control_binding_revision ==
                binding.control_binding_revision) {
          if (selected)
            throw std::invalid_argument("io_rule_stop_route_ambiguous");
          selected = &route;
        }
  if (!selected)
    throw std::invalid_argument("io_rule_stop_route_missing");
  return selected->control_route_id;
}
} // namespace

core::Result<std::vector<core::RetainedArtifactRefDTO>>
required_global_io_stop_artifacts(const RetainedIoPolicyProjection &rules,
                                  const RetainedBundle &bundle) {
  using Reply = core::Result<std::vector<core::RetainedArtifactRefDTO>>;
  try {
    std::map<core::ContentAddressedStoreKey, core::RetainedArtifactRefDTO> refs;
    for (const auto &rule : rules.activation_policies)
      for (const auto *binding : subjects(rule, bundle)) {
        const auto ref = required_io_stop_artifact_for_binding(
            bundle, binding->control_binding_id,
            binding->control_binding_revision);
        if (!ref.ok())
          return Reply::Err(ref.error());
        const auto [old, inserted] =
            refs.emplace(ref.value().content_addressed_store_key, ref.value());
        if (!inserted && old->second != ref.value())
          throw std::invalid_argument("io_rule_stop_product_ambiguous");
      }
    std::vector<core::RetainedArtifactRefDTO> result;
    for (auto &[key, ref] : refs) {
      (void)key;
      result.push_back(std::move(ref));
    }
    return Reply::Ok(std::move(result));
  } catch (const std::exception &error) {
    return Reply::Err(invalid(error));
  }
}

core::Result<void> validate_global_io_stop_entries(
    const RetainedIoPolicyProjection &rules, const RetainedBundle &bundle,
    const VerifiedRetainedModelMap *model_map,
    std::span<const PreparedActivationArtifact> raw_artifacts) {
  try {
    for (const auto &rule : rules.activation_policies)
      for (const auto *binding : subjects(rule, bundle)) {
        if (!model_map || model_map->query().retained_bundle_id !=
                              bundle.retained_bundle_id())
          throw std::invalid_argument("io_rule_stop_model_map_mismatch");
        const auto rows = read_qualified_io_stop_stage_rows(
            bundle, *model_map, binding->control_binding_id,
            binding->control_binding_revision, desired_route(*binding, bundle),
            raw_artifacts);
        if (!rows.ok())
          return core::Result<void>::Err(rows.error());
        const auto entry = core::ResolveIoRuleStopEntryIndex(
            rows.value(), rule.stop_entry_stage_id);
        if (!entry.ok())
          return core::Result<void>::Err(entry.error());
      }
    return core::Result<void>::Ok();
  } catch (const std::exception &error) {
    return core::Result<void>::Err(invalid(error));
  }
}
} // namespace manyforge::deployment
