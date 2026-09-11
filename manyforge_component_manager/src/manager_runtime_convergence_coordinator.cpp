#include "manyforge_component_manager/manager_runtime_convergence_coordinator.hpp"

#include <algorithm>
#include <exception>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace manyforge::component_manager {
namespace {

using manyforge::core::ActiveBundlePublicationGeneration;
using manyforge::core::ActiveBundlePublicationReceiptDTO;
using manyforge::core::ContentDigest;
using manyforge::core::RuntimeEpoch;
using manyforge::core::RuntimeRoleId;
using manyforge::core::RuntimeUnitId;
using manyforge::deployment::RetainedBundle;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerRuntimeConvergenceCoordinatorError(detail);
}

void VerifyApplyInputs(const ManagerApplyIntentDTO &apply_intent,
                       const RetainedBundle &predecessor,
                       const RetainedBundle &candidate) {
  if (!apply_intent.valid() ||
      !manyforge::deployment::verify_retained_bundle_identity(predecessor) ||
      !manyforge::deployment::verify_retained_bundle_identity(candidate)) {
    Fail("runtime convergence received an invalid Apply or retained bundle");
  }
  const auto &predecessor_graph = predecessor.value().cell_graph;
  const auto &candidate_graph = candidate.value().cell_graph;
  if (apply_intent.cell_id != predecessor_graph.cell_id ||
      apply_intent.cell_id != candidate_graph.cell_id ||
      apply_intent.expected_active_projection_revision.retained_bundle_id !=
          predecessor.retained_bundle_id() ||
      apply_intent.expected_active_projection_revision.graph_revision !=
          predecessor_graph.graph_revision ||
      apply_intent.candidate_retained_bundle_id !=
          candidate.retained_bundle_id() ||
      apply_intent.candidate_graph_revision != candidate_graph.graph_revision) {
    Fail("runtime convergence bundles do not match the exact Apply intent");
  }
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO VerifyCurrentPredecessor(
    const manyforge::core::IActiveBundlePublicationReader &reader,
    const ManagerApplyIntentDTO &apply_intent,
    const ActiveBundlePublicationGeneration publication_generation) {
  manyforge::core::Result<ActiveBundlePublicationReceiptDTO> read =
      manyforge::core::Result<ActiveBundlePublicationReceiptDTO>::Err(
          manyforge::core::ErrorCode::kInternalError);
  try {
    read = reader.ReadActiveBundlePublication(publication_generation);
  } catch (const std::exception &error) {
    Fail("current predecessor publication read failed: " +
         std::string(error.what()));
  } catch (...) {
    Fail("current predecessor publication read failed");
  }
  if (!read.ok()) {
    Fail("expected predecessor publication is not the current public head");
  }
  const auto &receipt = read.value();
  if (!receipt.valid() ||
      receipt.publication_generation != publication_generation ||
      receipt.request.candidate_projection_revision !=
          apply_intent.expected_active_projection_revision ||
      receipt.request.candidate_retained_bundle_id !=
          apply_intent.expected_active_projection_revision.retained_bundle_id ||
      receipt.request.candidate_graph_revision !=
          apply_intent.expected_active_projection_revision.graph_revision) {
    Fail("current predecessor publication differs from the Apply intent");
  }
  return receipt;
}

[[nodiscard]] RuntimeUnitConvergencePlanRowDTO MakeVerifiedPlanRow(
    const manyforge::core::RuntimeUnitDTO &runtime_unit,
    const manyforge::deployment::VerifiedRetainedRuntimeUnitProduct &product,
    const manyforge::core::RuntimeContractRequirementDTO &requirement,
    const manyforge::core::RuntimeContractOfferDigest &offer_digest,
    const RuntimeUnitCandidateDisposition disposition) {
  RuntimeUnitConvergencePlanRowDTO row{
      runtime_unit.runtime_unit_id,
      runtime_unit.runtime_profile_id,
      runtime_unit.runtime_profile_revision,
      manyforge::core::RuntimeLifecycleControl::kManagerManaged,
      disposition,
      product.product_id(),
      product.product_contract_id(),
      product.product_contract_revision(),
      product.product_identity(),
      product.configuration_identity(),
      runtime_unit.dependency_runtime_unit_ids,
      {RuntimeRoleId(std::string(product.runtime_role_id()))},
      manyforge::core::EndpointRoleId(
          std::string(product.status_endpoint_role_id())),
      std::string(product.status_endpoint_contract_id()),
      std::string(product.status_endpoint_contract_revision()),
      std::string(product.status_transport()),
      std::string(product.process_adapter_contract_id()),
      std::string(product.process_adapter_contract_revision()),
      std::string(product.retrieval_contract_id()),
      std::string(product.retrieval_contract_revision()),
      product.launchable_artifact_ref().content_digest,
      product.launchable_artifact_ref().byte_size,
      std::string(product.operating_system()),
      std::string(product.launch_environment_contract()),
      requirement,
      offer_digest,
  };
  if (!row.valid()) {
    Fail("verified runtime product produced an invalid convergence row");
  }
  return row;
}

[[nodiscard]] std::map<std::string, RuntimeUnitConvergencePlanRowDTO>
VerifyRuntimeProducts(
    const RetainedBundle &bundle,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    const std::string_view operating_system,
    const std::vector<std::string> &launch_environment_contracts) {
  std::map<std::string, RuntimeUnitConvergencePlanRowDTO> plans;
  for (const auto &runtime_unit : bundle.value().cell_graph.runtime_units) {
    const auto product = manyforge::deployment::verify_runtime_unit_product(
        bundle, runtime_unit.runtime_unit_id, artifact_source);
    if (!product.has_value()) {
      Fail("runtime unit has no I3-qualified I4 process adapter product: " +
           runtime_unit.runtime_unit_id.value());
    }
    if (product->operating_system() != operating_system) {
      Fail("runtime unit product is incompatible with the selected host OS: " +
           runtime_unit.runtime_unit_id.value());
    }
    if (!std::binary_search(launch_environment_contracts.begin(),
                            launch_environment_contracts.end(),
                            product->launch_environment_contract())) {
      Fail("runtime product is incompatible with the selected host launch "
           "environment: " +
           runtime_unit.runtime_unit_id.value());
    }
    const auto requirement_row = std::find_if(
        bundle.value().runtime_contract_requirements.begin(),
        bundle.value().runtime_contract_requirements.end(),
        [&](const auto &candidate) {
          return candidate.runtime_unit_id == runtime_unit.runtime_unit_id;
        });
    if (requirement_row ==
        bundle.value().runtime_contract_requirements.end()) {
      Fail("runtime unit has no retained contract-manifest requirement: " +
           runtime_unit.runtime_unit_id.value());
    }
    const auto *contract =
        manyforge::deployment::runtime_contract_declaration_registry()
            .find(runtime_unit.runtime_profile_id,
                  runtime_unit.runtime_profile_revision,
                  product->product_contract_id(),
                  product->product_contract_revision());
    if (contract == nullptr) {
      Fail("runtime unit has no selected component contract offer: " +
           runtime_unit.runtime_unit_id.value());
    }
    const auto compatible = manyforge::core::CheckRuntimeContractCompatibility(
        contract->offer(), requirement_row->requirement);
    if (!compatible.ok()) {
      Fail("runtime unit retained contract requirement is incompatible with "
           "its selected offer: " +
           runtime_unit.runtime_unit_id.value());
    }
    auto row =
        MakeVerifiedPlanRow(runtime_unit, *product, requirement_row->requirement,
                            contract->offer().offer_digest,
                            RuntimeUnitCandidateDisposition::kRetainedExact);
    if (!plans.emplace(runtime_unit.runtime_unit_id.value(), std::move(row))
             .second) {
      Fail("retained bundle contains a duplicate runtime-unit identity");
    }
  }
  return plans;
}

[[nodiscard]] bool CompatibleRuntimeSuccessor(
    const RuntimeUnitConvergencePlanRowDTO &predecessor,
    const RuntimeUnitConvergencePlanRowDTO &candidate) {
  // Each bundle was already verified against its own exact product, bindings,
  // endpoints, configuration, semantic requirement, and selected offer.
  // Runtime reports must still match the exact candidate facts before staged
  // readiness. Predecessor/successor continuity therefore owns only the stable
  // runtime-unit and lifecycle placement; implementation family, product and
  // executable identities are diagnostic/replacement facts, not compatibility.
  return predecessor.runtime_unit_id == candidate.runtime_unit_id &&
         predecessor.lifecycle_control == candidate.lifecycle_control;
}

[[nodiscard]] std::vector<RuntimeUnitConvergencePlanRowDTO> BuildCandidatePlan(
    const std::map<std::string, RuntimeUnitConvergencePlanRowDTO>
        &predecessor_plans,
    const RetainedBundle &candidate,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    const std::string_view operating_system,
    const std::vector<std::string> &launch_environment_contracts) {
  auto candidate_plans =
      VerifyRuntimeProducts(candidate, artifact_source, operating_system,
                            launch_environment_contracts);
  for (const auto &[runtime_id, predecessor_row] : predecessor_plans) {
    const auto candidate_row = candidate_plans.find(runtime_id);
    if (candidate_row == candidate_plans.end()) {
      Fail("runtime-unit removal is not qualified by the current I4 "
           "command-surface retirement boundary: " +
           runtime_id);
    }
    static_cast<void>(predecessor_row);
  }
  std::vector<RuntimeUnitConvergencePlanRowDTO> result;
  result.reserve(candidate_plans.size());
  for (auto &[runtime_id, candidate_row] : candidate_plans) {
    const auto predecessor_row = predecessor_plans.find(runtime_id);
    if (predecessor_row == predecessor_plans.end()) {
      candidate_row.candidate_disposition =
          RuntimeUnitCandidateDisposition::kAdded;
    } else if (!CompatibleRuntimeSuccessor(predecessor_row->second,
                                           candidate_row)) {
      Fail("runtime-unit replacement is not qualified by the current I4 "
           "checkpoint: " +
           runtime_id);
    }
    result.push_back(std::move(candidate_row));
  }
  return result;
}

[[nodiscard]] std::vector<std::size_t>
DependencyOrder(const std::vector<RuntimeUnitConvergencePlanRowDTO> &plans) {
  std::map<std::string, std::size_t> indexes;
  for (std::size_t index = 0U; index < plans.size(); ++index) {
    indexes.emplace(plans[index].runtime_unit_id.value(), index);
  }
  std::vector<std::size_t> indegree(plans.size(), 0U);
  std::vector<std::vector<std::size_t>> dependents(plans.size());
  for (std::size_t index = 0U; index < plans.size(); ++index) {
    for (const auto &dependency : plans[index].dependency_runtime_unit_ids) {
      const auto found = indexes.find(dependency.value());
      if (found == indexes.end()) {
        Fail("runtime convergence plan has an undeclared dependency");
      }
      ++indegree[index];
      dependents[found->second].push_back(index);
    }
  }
  std::set<std::pair<std::string, std::size_t>> ready;
  for (std::size_t index = 0U; index < plans.size(); ++index) {
    if (indegree[index] == 0U) {
      ready.emplace(plans[index].runtime_unit_id.value(), index);
    }
  }
  std::vector<std::size_t> order;
  order.reserve(plans.size());
  while (!ready.empty()) {
    const auto current = ready.begin()->second;
    ready.erase(ready.begin());
    order.push_back(current);
    for (const auto dependent : dependents[current]) {
      if (--indegree[dependent] == 0U) {
        ready.emplace(plans[dependent].runtime_unit_id.value(), dependent);
      }
    }
  }
  if (order.size() != plans.size()) {
    Fail("runtime convergence plan dependency graph is cyclic");
  }
  return order;
}

[[nodiscard]] RuntimeLifecycleControl ToObservationLifecycle(
    const manyforge::core::RuntimeLifecycleControl lifecycle) {
  switch (lifecycle) {
  case manyforge::core::RuntimeLifecycleControl::kManagerManaged:
    return RuntimeLifecycleControl::kManagerManaged;
  case manyforge::core::RuntimeLifecycleControl::kExternallyManaged:
    return RuntimeLifecycleControl::kExternallyManaged;
  }
  Fail("runtime convergence plan has an unknown lifecycle owner");
}

void IngestObservedPresence(RuntimeObservationReducer &reducer,
                            const RuntimeUnitConvergenceQueryDTO &query,
                            const ProcessPresenceObservation &sample) {
  if (sample.runtime_unit_id() != query.plan.runtime_unit_id.value() ||
      !reducer.ObserveProcess(sample)) {
    Fail("process port returned rejected or foreign presence evidence");
  }
}

void IngestEnsuredPresence(RuntimeObservationReducer &reducer,
                           const RuntimeUnitConvergenceQueryDTO &query,
                           const ProcessPresenceObservation &before,
                           const ProcessPresenceObservation &after) {
  if (after.runtime_unit_id() != query.plan.runtime_unit_id.value()) {
    throw std::invalid_argument(
        "process Ensure returned foreign presence evidence");
  }
  const bool accepted =
      after.observer_instance_id() == before.observer_instance_id()
          ? reducer.ObserveProcess(after)
          : reducer.ReplaceProcessObserver(after);
  if (!accepted) {
    throw std::invalid_argument(
        "process Ensure returned rejected presence evidence");
  }
}

[[nodiscard]] ProcessPresenceObservation
ObserveProcessSafely(IRuntimeUnitProcessPort &port,
                     const RuntimeUnitConvergenceQueryDTO &query) {
  try {
    return port.ObserveProcess(query);
  } catch (const std::exception &error) {
    Fail("runtime process observation failed: " + std::string(error.what()));
  } catch (...) {
    Fail("runtime process observation failed");
  }
}

[[nodiscard]] std::optional<RuntimeUnitSourceReport>
ObserveProgressSafely(IRuntimeUnitProgressPort &port,
                      const RuntimeUnitConvergenceQueryDTO &query,
                      const RuntimeEpoch &runtime_epoch) {
  try {
    return port.ObserveProgress(query, runtime_epoch);
  } catch (const std::exception &error) {
    Fail("runtime progress observation failed: " + std::string(error.what()));
  } catch (...) {
    Fail("runtime progress observation failed");
  }
}

[[nodiscard]] RuntimeUnitExpectation
MakeExpectation(const RuntimeUnitConvergencePlanRowDTO &plan) {
  std::vector<std::string> dependencies;
  dependencies.reserve(plan.dependency_runtime_unit_ids.size());
  for (const auto &dependency : plan.dependency_runtime_unit_ids) {
    dependencies.push_back(dependency.value());
  }
  std::vector<std::string> roles;
  roles.reserve(plan.required_runtime_role_ids.size());
  for (const auto &role : plan.required_runtime_role_ids) {
    roles.push_back(role.value());
  }
  return RuntimeUnitExpectation(
      plan.runtime_unit_id.value(),
      ToObservationLifecycle(plan.lifecycle_control),
      plan.product_identity.value(), plan.configuration_identity.value(),
      kManagerRuntimeMaximumPresenceAgeNs, kManagerRuntimeMaximumReportAgeNs,
      std::nullopt, std::move(dependencies), std::move(roles),
      ManagedCommandSurfaceState::kClosedNoActiveEffect,
      plan.runtime_contract_offer_digest.value());
}

void RequireRuntimeContractAdmission(
    const RuntimeUnitConvergencePlanRowDTO &plan) {
  try {
    VerifyRuntimeContractAdmission(plan);
  } catch (const std::exception &error) {
    Fail(error.what());
  } catch (...) {
    Fail("runtime convergence contract admission failed");
  }
}

[[nodiscard]] RuntimeUnitStagedReadyEvidenceDTO
MakeStagedEvidence(const RuntimeUnitConvergencePlanRowDTO &plan,
                   const RuntimeUnitObservation &observation) {
  if (!observation.converged() ||
      !observation.process_observation_sequence().has_value() ||
      !observation.source_report_sequence().has_value() ||
      !observation.critical_progress_sequence().has_value() ||
      !observation.source_phase().has_value() ||
      !observation.source_health().has_value() ||
      !observation.command_surface_state().has_value() ||
      !observation.presence_age_ns().has_value() ||
      !observation.report_age_ns().has_value() ||
      !observation.progress_age_ns().has_value() ||
      observation.observed_runtime_contract_offer_digest() !=
          plan.runtime_contract_offer_digest.value()) {
    Fail("converged runtime observation lacks complete staged-ready evidence");
  }
  std::vector<RuntimeRoleId> roles;
  roles.reserve(observation.ready_runtime_role_ids().size());
  for (const auto &role : observation.ready_runtime_role_ids()) {
    roles.emplace_back(role);
  }
  std::vector<RuntimeUnitDependencyEpochDTO> dependency_epochs;
  dependency_epochs.reserve(observation.dependency_runtime_epochs().size());
  for (const auto &[runtime_id, runtime_epoch] :
       observation.dependency_runtime_epochs()) {
    dependency_epochs.push_back(
        {RuntimeUnitId(runtime_id), RuntimeEpoch(runtime_epoch)});
  }
  RuntimeUnitStagedReadyEvidenceDTO evidence{
      plan.runtime_unit_id,
      plan.lifecycle_control,
      plan.candidate_disposition,
      RuntimeEpoch(observation.runtime_epoch()),
      observation.process_observer_instance_id(),
      *observation.process_observation_sequence(),
      *observation.source_report_sequence(),
      *observation.critical_progress_sequence(),
      ContentDigest(observation.observed_product_identity()),
      ContentDigest(observation.observed_configuration_identity()),
      *observation.source_phase(),
      *observation.source_health(),
      std::move(roles),
      *observation.command_surface_state(),
      std::move(dependency_epochs),
      *observation.presence_age_ns(),
      *observation.report_age_ns(),
      *observation.progress_age_ns(),
      manyforge::core::RuntimeContractOfferDigest(
          observation.observed_runtime_contract_offer_digest()),
  };
  if (!evidence.valid()) {
    Fail("Manager produced invalid staged-ready runtime evidence");
  }
  return evidence;
}

[[nodiscard]] bool
SameCurrentStagedEvidence(const ManagerApplyRuntimeStagedReadyDTO &left,
                          const ManagerApplyRuntimeStagedReadyDTO &right) {
  return left.apply_id == right.apply_id &&
         left.apply_generation == right.apply_generation &&
         left.runtime_convergence_intent_digest ==
             right.runtime_convergence_intent_digest &&
         left.predecessor_publication_generation ==
             right.predecessor_publication_generation &&
         left.predecessor_public_pointer_digest ==
             right.predecessor_public_pointer_digest &&
         left.evaluated_at_ns == right.evaluated_at_ns &&
         left.runtime_units == right.runtime_units;
}

} // namespace

std::vector<RuntimeUnitConvergencePlanRowDTO>
BuildVerifiedRuntimeConvergencePlan(
    const manyforge::deployment::RetainedBundle &bundle,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    const std::string_view operating_system,
    std::vector<std::string> launch_environment_contracts) {
  std::sort(launch_environment_contracts.begin(),
            launch_environment_contracts.end());
  if (operating_system.empty() || launch_environment_contracts.empty() ||
      std::any_of(launch_environment_contracts.begin(),
                  launch_environment_contracts.end(),
                  [](const auto &value) { return value.empty(); }) ||
      std::adjacent_find(launch_environment_contracts.begin(),
                         launch_environment_contracts.end()) !=
          launch_environment_contracts.end()) {
    throw std::invalid_argument(
        "runtime convergence plan host contracts are invalid");
  }
  auto plans = VerifyRuntimeProducts(bundle, artifact_source, operating_system,
                                     launch_environment_contracts);
  std::vector<RuntimeUnitConvergencePlanRowDTO> result;
  result.reserve(plans.size());
  for (auto &[unused, row] : plans) {
    static_cast<void>(unused);
    result.push_back(std::move(row));
  }
  return result;
}

ManagerRuntimeProcessEffectUncertainError::
    ManagerRuntimeProcessEffectUncertainError(
        std::string detail, RuntimeUnitConvergenceQueryDTO recovery_query)
    : ManagerRuntimeConvergenceCoordinatorError(std::move(detail)),
      recovery_query_(std::move(recovery_query)) {}

const RuntimeUnitConvergenceQueryDTO &
ManagerRuntimeProcessEffectUncertainError::recovery_query() const noexcept {
  return recovery_query_;
}

ManagerApplyRuntimeConvergenceCoordinator::
    ManagerApplyRuntimeConvergenceCoordinator(
        ManagerApplyFenceCoordinator &fence_coordinator,
        SqliteManagerApplyJournal &apply_journal,
        const manyforge::core::IActiveBundlePublicationReader
            &publication_reader,
        const manyforge::deployment::IVerifiedRetainedProductArtifactSource
            &artifact_source,
        IRuntimeUnitProcessPort &process_port,
        IRuntimeUnitProgressPort &progress_port, std::string operating_system,
        std::string shell_contract, RuntimeObservationReducer::Clock clock_ns,
        RuntimePreStartGuard pre_start_guard)
    : ManagerApplyRuntimeConvergenceCoordinator(
          fence_coordinator, apply_journal, publication_reader, artifact_source,
          process_port, progress_port, std::move(operating_system),
          std::vector<std::string>{std::move(shell_contract)},
          std::move(clock_ns), std::move(pre_start_guard)) {}

ManagerApplyRuntimeConvergenceCoordinator::
    ManagerApplyRuntimeConvergenceCoordinator(
        ManagerApplyFenceCoordinator &fence_coordinator,
        SqliteManagerApplyJournal &apply_journal,
        const manyforge::core::IActiveBundlePublicationReader
            &publication_reader,
        const manyforge::deployment::IVerifiedRetainedProductArtifactSource
            &artifact_source,
        IRuntimeUnitProcessPort &process_port,
        IRuntimeUnitProgressPort &progress_port, std::string operating_system,
        std::vector<std::string> launch_environment_contracts,
        RuntimeObservationReducer::Clock clock_ns,
        RuntimePreStartGuard pre_start_guard)
    : fence_coordinator_(fence_coordinator), apply_journal_(apply_journal),
      publication_reader_(publication_reader),
      artifact_source_(artifact_source), process_port_(process_port),
      progress_port_(progress_port),
      operating_system_(std::move(operating_system)),
      launch_environment_contracts_(std::move(launch_environment_contracts)),
      clock_ns_(std::move(clock_ns)),
      pre_start_guard_(std::move(pre_start_guard)) {
  std::sort(launch_environment_contracts_.begin(),
            launch_environment_contracts_.end());
  if (operating_system_.empty() || launch_environment_contracts_.empty() ||
      std::any_of(launch_environment_contracts_.begin(),
                  launch_environment_contracts_.end(),
                  [](const auto &value) { return value.empty(); }) ||
      std::adjacent_find(launch_environment_contracts_.begin(),
                         launch_environment_contracts_.end()) !=
          launch_environment_contracts_.end() ||
      !clock_ns_) {
    throw std::invalid_argument(
        "runtime convergence coordinator dependencies are invalid");
  }
  if (!pre_start_guard_) {
    pre_start_guard_ = [] {};
  }
}

ManagerApplyRuntimeConvergenceResult
ManagerApplyRuntimeConvergenceCoordinator::ConvergeOrRecover(
    const ManagerApplyIntentDTO &apply_intent,
    const RetainedBundle &predecessor, const RetainedBundle &candidate,
    const ManagerApplyFenceExpectationDTO &fence_expectation,
    const ActiveBundlePublicationGeneration
        predecessor_publication_generation) {
  std::lock_guard operation_lock(operation_mutex_);

  VerifyApplyInputs(apply_intent, predecessor, candidate);
  const auto initial_publication = VerifyCurrentPredecessor(
      publication_reader_, apply_intent, predecessor_publication_generation);
  const auto predecessor_plans =
      VerifyRuntimeProducts(predecessor, artifact_source_, operating_system_,
                            launch_environment_contracts_);
  auto runtime_plans =
      BuildCandidatePlan(predecessor_plans, candidate, artifact_source_,
                         operating_system_, launch_environment_contracts_);
  const auto dependency_order = DependencyOrder(runtime_plans);

  auto fence = fence_coordinator_.AcquireOrRecoverFence(
      apply_intent, predecessor, candidate, fence_expectation);
  const auto after_fence_publication = VerifyCurrentPredecessor(
      publication_reader_, apply_intent, predecessor_publication_generation);
  if (after_fence_publication != initial_publication) {
    Fail("predecessor publication changed while acquiring the Apply fence");
  }

  ManagerApplyRuntimeConvergenceIntentDTO convergence_intent{
      apply_intent.apply_id,
      apply_intent.apply_generation,
      candidate.retained_bundle_id(),
      candidate.value().cell_graph.graph_revision,
      fence.durable_acceptance.record.acceptance_digest,
      initial_publication.publication_generation,
      initial_publication.durable_public_pointer_content_digest,
      std::move(runtime_plans),
  };
  const auto durable_intent = apply_journal_.EnsureRuntimeConvergenceIntent(
      fence.durable_intent.record.intent_digest, convergence_intent);
  const auto prior_staged_ready = apply_journal_.ReadLatestRuntimeStagedReady(
      apply_intent.apply_id, apply_intent.apply_generation);
  const auto prior_proves_candidate_preparation =
      [&](const RuntimeUnitConvergencePlanRowDTO &plan,
          const ProcessPresenceObservation &process) {
        if (!prior_staged_ready.has_value() ||
            prior_staged_ready->staged_ready.runtime_convergence_intent_digest !=
                durable_intent.record.runtime_convergence_intent_digest ||
            process.state() != ProcessPresenceState::kPresent) {
          return false;
        }
        const auto &rows = prior_staged_ready->staged_ready.runtime_units;
        const auto found = std::lower_bound(
            rows.begin(), rows.end(), plan.runtime_unit_id,
            [](const auto &row, const auto &runtime_unit_id) {
              return row.runtime_unit_id < runtime_unit_id;
            });
        return found != rows.end() &&
               found->runtime_unit_id == plan.runtime_unit_id &&
               found->runtime_epoch.value() ==
                   process.process_incarnation() &&
               found->observed_product_identity == plan.product_identity &&
               found->observed_configuration_identity ==
                   plan.configuration_identity &&
               found->observed_runtime_contract_offer_digest ==
                   plan.runtime_contract_offer_digest;
      };

  std::vector<std::string> declared_runtime_ids;
  std::vector<RuntimeUnitExpectation> expectations;
  declared_runtime_ids.reserve(convergence_intent.runtime_units.size());
  expectations.reserve(convergence_intent.runtime_units.size());
  for (const auto &plan : convergence_intent.runtime_units) {
    declared_runtime_ids.push_back(plan.runtime_unit_id.value());
    expectations.push_back(MakeExpectation(plan));
  }
  RuntimeObservationReducer reducer(clock_ns_, std::move(declared_runtime_ids));
  std::map<std::string, manyforge::core::ManagedRuntimeWithdrawalDTO> withdrawals;

  for (const auto index : dependency_order) {
    const auto &plan = convergence_intent.runtime_units[index];
    RuntimeUnitConvergenceQueryDTO query{
        apply_intent.apply_id,
        apply_intent.apply_generation,
        candidate.retained_bundle_id(),
        candidate.value().cell_graph.graph_revision,
        durable_intent.record.runtime_convergence_intent_digest,
        plan,
    };
    const auto old = predecessor_plans.find(plan.runtime_unit_id.value());
    if (old != predecessor_plans.end() &&
        plan.lifecycle_control ==
            manyforge::core::RuntimeLifecycleControl::kManagerManaged &&
        (old->second.product_identity != plan.product_identity ||
         old->second.configuration_identity != plan.configuration_identity ||
         old->second.runtime_contract_offer_digest !=
             plan.runtime_contract_offer_digest)) {
      query.replacement = fence.rehydrate_request;
    }
    if (!query.valid()) {
      Fail("Manager produced an invalid runtime convergence query");
    }
    ProcessPresenceObservation process =
        ObserveProcessSafely(process_port_, query);
    IngestObservedPresence(reducer, query, process);
    if (plan.lifecycle_control ==
            manyforge::core::RuntimeLifecycleControl::kManagerManaged &&
        !prior_proves_candidate_preparation(plan, process)) {
      // Re-read the durable row's sealed requirement immediately before the
      // only managed lifecycle effect.  A journal replay cannot bypass this
      // admission by reusing an old process query.
      RequireRuntimeContractAdmission(query.plan);
      // This Manager-owned generation check is deliberately outside the
      // effect-uncertain catch: a typed restart requirement is certain.
      pre_start_guard_();
      try {
        // A surviving exact process still has candidate-scoped staged-ready
        // work to reconcile (for example, preparing newly retained native
        // projection bindings). EnsureRunning is idempotent for the exact
        // query and is the effectful convergence boundary; presence alone
        // proves only process identity, not candidate readiness.
        const auto ensured = process_port_.EnsureRunning(query);
        IngestEnsuredPresence(reducer, query, process, ensured);
        process = ensured;
      } catch (const std::exception &error) {
        throw ManagerRuntimeProcessEffectUncertainError(
            "managed runtime Ensure outcome is uncertain: " +
                std::string(error.what()),
            query);
      } catch (...) {
        throw ManagerRuntimeProcessEffectUncertainError(
            "managed runtime Ensure outcome is uncertain", query);
      }
    }
    if (query.replacement) {
      auto withdrawal = process_port_.ReadPredecessorWithdrawal(query);
      if (!withdrawal && prior_staged_ready) {
        const auto &prior_rows = prior_staged_ready->staged_ready.runtime_units;
        const auto prior = std::find_if(
            prior_rows.begin(), prior_rows.end(), [&](const auto &row) {
              return row.runtime_unit_id == plan.runtime_unit_id;
            });
        if (prior != prior_rows.end())
          withdrawal = prior->predecessor_withdrawal;
      }
      if (withdrawal) {
        if (!withdrawal->valid() ||
            withdrawal->apply_id != apply_intent.apply_id ||
            withdrawal->apply_generation != apply_intent.apply_generation ||
            withdrawal->runtime_convergence_intent_digest !=
                durable_intent.record.runtime_convergence_intent_digest ||
            withdrawal->runtime_unit_id != plan.runtime_unit_id ||
            withdrawal->product_identity != old->second.product_identity ||
            withdrawal->configuration_identity !=
                old->second.configuration_identity ||
            withdrawal->runtime_epoch.value() == process.process_incarnation())
          Fail("predecessor withdrawal differs from the exact fenced "
               "replacement");
        withdrawals.emplace(plan.runtime_unit_id.value(), *withdrawal);
      }
    }
    if (process.state() == ProcessPresenceState::kPresent) {
      const RuntimeEpoch runtime_epoch(process.process_incarnation());
      const auto report =
          ObserveProgressSafely(progress_port_, query, runtime_epoch);
      if (report.has_value()) {
        if (report->offered_runtime_contract_offer_digest() !=
            query.plan.runtime_contract_offer_digest.value()) {
          Fail("progress report offered runtime contract differs from the "
               "selected package offer");
        }
        if (!reducer.IngestSourceReport(*report)) {
          Fail("progress port returned rejected or foreign source evidence");
        }
      }
    }
  }

  auto snapshot = reducer.Evaluate(expectations);
  const bool all_converged = std::all_of(
      snapshot.observations().begin(), snapshot.observations().end(),
      [](const auto &observation) { return observation.converged(); });
  if (!all_converged) {
    return {
        std::move(fence),
        durable_intent,
        std::move(snapshot),
        std::nullopt,
        ManagerRuntimeConvergenceResultState::kPending,
    };
  }

  const auto final_publication = VerifyCurrentPredecessor(
      publication_reader_, apply_intent, predecessor_publication_generation);
  if (final_publication != initial_publication) {
    Fail("predecessor publication changed before runtime staged-ready commit");
  }

  std::vector<RuntimeUnitStagedReadyEvidenceDTO> staged_rows;
  staged_rows.reserve(snapshot.observations().size());
  for (const auto &observation : snapshot.observations()) {
    const auto found = std::lower_bound(
        convergence_intent.runtime_units.begin(),
        convergence_intent.runtime_units.end(), observation.runtime_unit_id(),
        [](const auto &plan, const auto &runtime_id) {
          return plan.runtime_unit_id.value() < runtime_id;
        });
    if (found == convergence_intent.runtime_units.end() ||
        found->runtime_unit_id.value() != observation.runtime_unit_id()) {
      Fail("runtime reducer returned an undeclared observation");
    }
    staged_rows.push_back(MakeStagedEvidence(*found, observation));
    const auto withdrawal = withdrawals.find(observation.runtime_unit_id());
    if (withdrawal != withdrawals.end())
      staged_rows.back().predecessor_withdrawal = withdrawal->second;
  }
  std::sort(staged_rows.begin(), staged_rows.end(),
            [](const auto &left, const auto &right) {
              return left.runtime_unit_id < right.runtime_unit_id;
            });

  const auto latest = apply_journal_.ReadLatestRuntimeStagedReady(
      apply_intent.apply_id, apply_intent.apply_generation);
  ManagerApplyRuntimeStagedReadyDTO staged_ready{
      apply_intent.apply_id,
      apply_intent.apply_generation,
      latest.has_value() ? latest->staged_ready.staged_ready_generation + 1U
                         : 1U,
      durable_intent.record.runtime_convergence_intent_digest,
      initial_publication.publication_generation,
      initial_publication.durable_public_pointer_content_digest,
      snapshot.evaluated_at_ns(),
      std::move(staged_rows),
  };
  const auto durable_staged_ready =
      latest.has_value() &&
              SameCurrentStagedEvidence(latest->staged_ready, staged_ready)
          ? ManagerApplyRuntimeStagedReadyWriteResult{*latest, false}
          : apply_journal_.EnsureRuntimeStagedReady(staged_ready);

  return {
      std::move(fence),
      durable_intent,
      std::move(snapshot),
      std::move(durable_staged_ready),
      ManagerRuntimeConvergenceResultState::kStagedReady,
  };
}

} // namespace manyforge::component_manager
