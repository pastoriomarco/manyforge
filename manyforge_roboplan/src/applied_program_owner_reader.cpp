#include "applied_program_owner_reader.hpp"

#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <manyforge_deployment/retained_io_policy_compiler.hpp>
#include <map>

namespace manyforge::roboplan_backend::detail {

core::Result<core::AppliedProgramBlackboardContextDTO>
CompileAppliedProgramBlackboardContext(
    const AppliedProgramOwnerSnapshot &applied,
    std::shared_ptr<const deployment::ProgramBindingContract> program,
    std::shared_ptr<const deployment::ProgramBinding> binding,
    const deployment::RetainedBundle &bundle) {
  using Reply = core::Result<core::AppliedProgramBlackboardContextDTO>;
  const auto rejected = [](std::string reason) {
    return Reply::Err(
        {"program_applied_selection_unavailable",
         "The actual Applied Program differs from its sealed source products",
         {{"reason", std::move(reason)}}});
  };
  if (!applied.publication_generation.nonzero() || !program || !binding ||
      program->value().program_id != applied.program_id ||
      program->value().program_revision != applied.program_revision ||
      binding->program_binding_id() != applied.program_binding_id ||
      binding->value().program_id != applied.program_id ||
      binding->value().program_revision != applied.program_revision ||
      binding->value().retained_bundle_id != bundle.retained_bundle_id() ||
      binding->value().graph_revision !=
          bundle.value().cell_graph.graph_revision)
    return rejected("program_bb_applied_product_stale");
  auto conditions =
      deployment::compile_program_io_conditions(*program, *binding, bundle);
  if (!conditions.ok())
    return Reply::Err(conditions.error());
  auto policies =
      deployment::compile_retained_io_policies(*program, *binding, bundle);
  if (!policies.ok())
    return Reply::Err(policies.error());
  std::map<std::string, core::IoConditionValueKind> keys;
  const auto append = [&](const auto &input) {
    for (const auto &key : input) {
      const auto [found, inserted] = keys.emplace(key.name, key.value_kind);
      if (!inserted && found->second != key.value_kind)
        return false;
    }
    return true;
  };
  if (!append(conditions.value().referenced_program_blackboard_keys) ||
      !append(policies.value().referenced_deployment_blackboard_keys))
    return rejected("program_bb_required_key_type_mismatch");
  std::vector<core::IoBlackboardKeyDescriptorDTO> required;
  required.reserve(keys.size());
  for (const auto &[name, kind] : keys)
    required.push_back({name, kind});
  const auto actual_binding = std::shared_ptr<const core::ProgramBindingDTO>(
      binding, &binding->value());
  const auto declarations = std::shared_ptr<
      const std::vector<core::ProgramBlackboardKeyDeclarationDTO>>(
      program, &program->blackboard_key_declarations());
  return Reply::Ok(
      {applied.publication_generation,
       applied.program_id, applied.program_revision, applied.program_binding_id,
       bundle.retained_bundle_id(), bundle.value().cell_graph.graph_revision,
       actual_binding, declarations,
       std::make_shared<const core::ProgramIoConditionProjectionDTO>(
           std::move(conditions.value())),
       std::move(required), core::kMaximumCellAdmissionJsonRequestBytes});
}

} // namespace manyforge::roboplan_backend::detail
