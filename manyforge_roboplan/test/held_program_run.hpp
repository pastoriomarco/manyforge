#pragma once

// Program work names the Program run the Kernel holds. This helper makes a
// run held the way the Program runner starts one: ready cell admission, the
// run's Program-cycle claim, the run's capture and Run of every group. The
// captured Program has one motion node on the participant's target; work of
// the run names that node in its participants' execution scope.

#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/testing/request_key_fixture.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/manager_liveness.hpp>
#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/program_execution_control.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// The captured node of the held run's motion and its first attempt.
[[nodiscard]] inline core::ProgramNodeExecutionScopeDTO HeldProgramRunMotionScope() {
  return {core::ProgramNodeInstanceId("held_run.motion"), core::NodeAttemptNumber(1U)};
}

/// `config` able to admit a Program-cycle claim: a test cell has no Manager
/// liveness receivers, so its required set is the empty one.
[[nodiscard]] inline core::ResourceAddressedExecutionKernelConfig ProgramCycleReadyConfig(
    core::ResourceAddressedExecutionKernelConfig config,
    const core::RetainedBundleId &retained_bundle_id) {
  config.manager_liveness_required_set = core::ManagerLivenessRequiredSetDTO{
      config.cell_id, retained_bundle_id, core::ComputeRequiredRuntimeSetDigest({}),
      core::RuntimeUnitId("manager.held-run"), {}};
  return config;
}

/// Makes `run` the Program run `kernel` holds for `binding`'s Program, with
/// one captured motion node (`HeldProgramRunMotionScope`) on the binding's
/// target and capability, and runs every group. The Kernel's config must be
/// `ProgramCycleReadyConfig`. Throws with the Kernel's refusal.
inline void HoldProgramRun(core::ResourceAddressedExecutionKernel &kernel,
                           const core::RuntimeParticipantBinding &binding,
                           const core::ProgramRunNumber run) {
  const auto &value = binding.value();
  const auto require = [](const auto &result, const char *step) {
    if (!result.ok())
      throw std::logic_error(std::string("held Program run: ") + step + ": " +
                             result.error().message);
  };
  const core::CellAdmissionReadinessSnapshotDTO ready{
      true,
      core::ProgramCycleStatusDTO{1U, core::ProgramCycleStateDTO::kIdleAfterReset, std::nullopt,
                                  std::nullopt, std::nullopt},
      true, 1U, 1U, true, true, 0U, 0U, 0U};
  require(kernel.UpdateCellAdmissionReadiness(ready), "readiness");
  const auto suffix = std::to_string(run.value());
  const auto claim = kernel.AcquireCellAdmissionClaim(core::CellAdmissionClaimIntentDTO{
      core::CellAdmissionClaimKindDTO::kProgramCycle,
      core::ApplicationRole::kProgramRunner,
      core::CellAdmissionOwnerTransactionId(run),
      core::TestRequestKey("request.held-run.claim." + suffix),
      kernel.GetAuthoringAdmission().admission_generation,
      {},
      core::ProgramCycleAdmissionEvidenceDTO{value.program_revision, value.program_revision},
      std::nullopt});
  require(claim, "claim");

  const core::ProgramTargetReferenceDTO target{value.controllable_component_id,
                                               value.command_target_id};
  // The target names its resources by role, as a sealed Program Binding does.
  std::vector<core::ProgramResourceRoleBindingDTO> roles;
  for (const auto &resource : value.resources)
    roles.push_back({resource.resource_role_id, {resource.resource_id},
                     {resource.compiled_resource_name}});
  const core::ContentDigest digest("sha256:" + std::string(64U, '0'));
  core::ProgramBindingDTO program_binding{
      std::string(core::kProgramBindingApiVersion),
      value.program_binding_id,
      value.program_revision,
      value.retained_bundle_id,
      value.graph_revision,
      {"test.catalog", "1", digest, digest},
      {}, {}, {}, {}, {},
      {digest, {}, {}}};
  program_binding.target_bindings.push_back(
      core::ProgramTargetBindingDTO{target, {}, {}, {}, std::move(roles), {}});
  auto table = std::make_shared<core::ProgramExecutionTableDTO>();
  table->nodes.push_back(core::ProgramExecutionNodeDTO{
      HeldProgramRunMotionScope().node_instance_id, "kernel.operation",
      {{target, value.capability_id, true}}});
  require(kernel.register_program_run_capture(core::ProgramRunExecutionCaptureDTO{
              claim.value().claim_generation, run, value.program_revision,
              value.program_binding_id, value.retained_bundle_id, value.graph_revision,
              std::move(table),
              std::make_shared<const core::ProgramBindingDTO>(std::move(program_binding))}),
          "capture");

  const auto scope = kernel.get_program_execution_scope(run);
  require(scope, "scope");
  require(kernel.release_program_selection(
              core::ProgramExecutionSelectionRequestDTO{
                  {core::TestRequestKey("request.held-run.run." + suffix), "test"},
                  run,
                  {"all"},
                  {}},
              scope.value().hold_revision),
          "run");
}

}  // namespace manyforge::roboplan_backend
