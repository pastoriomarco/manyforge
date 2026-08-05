#include "manyforge_component_manager/manager_active_runtime_coordinator.hpp"

#include "manyforge_component_manager/manager_runtime_convergence_coordinator.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace manyforge::component_manager {
namespace {

using manyforge::core::NamedApplyPublicationTransactionDTO;
using manyforge::core::RuntimeEpoch;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerActiveRuntimeCoordinatorError(detail);
}

[[nodiscard]] std::vector<std::size_t>
DependencyOrder(const std::vector<RuntimeUnitConvergencePlanRowDTO> &plans) {
  std::map<std::string, std::size_t> indexes;
  for (std::size_t index = 0U; index < plans.size(); ++index) {
    if (!indexes.emplace(plans[index].runtime_unit_id.value(), index).second) {
      Fail("active runtime plan contains a duplicate runtime-unit identity");
    }
  }
  std::vector<std::size_t> indegree(plans.size(), 0U);
  std::vector<std::vector<std::size_t>> dependents(plans.size());
  for (std::size_t index = 0U; index < plans.size(); ++index) {
    for (const auto &dependency : plans[index].dependency_runtime_unit_ids) {
      const auto found = indexes.find(dependency.value());
      if (found == indexes.end()) {
        Fail("active runtime plan contains an undeclared dependency");
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
    Fail("active runtime plan dependency graph is cyclic");
  }
  return order;
}

[[nodiscard]] RuntimeUnitConvergenceQueryDTO
MakeQuery(const ManagerActiveRuntimePlanDTO &plan,
          const RuntimeUnitConvergencePlanRowDTO &row) {
  const auto *transaction = std::get_if<NamedApplyPublicationTransactionDTO>(
      &plan.active_publication.request.transaction_ref);
  if (transaction == nullptr ||
      !plan.runtime_convergence_intent_digest.has_value()) {
    Fail("active runtime query requires a named Apply plan");
  }
  RuntimeUnitConvergenceQueryDTO query{
      transaction->apply_id,
      transaction->apply_generation,
      plan.active_publication.request.candidate_retained_bundle_id,
      plan.active_publication.request.candidate_graph_revision,
      *plan.runtime_convergence_intent_digest,
      row,
  };
  if (!query.valid()) {
    Fail("Manager produced an invalid active runtime query");
  }
  return query;
}

[[nodiscard]] RuntimeLifecycleControl ToObservationLifecycle(
    const manyforge::core::RuntimeLifecycleControl lifecycle) {
  switch (lifecycle) {
  case manyforge::core::RuntimeLifecycleControl::kManagerManaged:
    return RuntimeLifecycleControl::kManagerManaged;
  case manyforge::core::RuntimeLifecycleControl::kExternallyManaged:
    return RuntimeLifecycleControl::kExternallyManaged;
  }
  Fail("active runtime plan has an unknown lifecycle owner");
}

[[nodiscard]] RuntimeUnitExpectation
MakeExpectation(const RuntimeUnitConvergencePlanRowDTO &row) {
  std::vector<std::string> dependencies;
  dependencies.reserve(row.dependency_runtime_unit_ids.size());
  for (const auto &dependency : row.dependency_runtime_unit_ids) {
    dependencies.push_back(dependency.value());
  }
  std::vector<std::string> roles;
  roles.reserve(row.required_runtime_role_ids.size());
  for (const auto &role : row.required_runtime_role_ids) {
    roles.push_back(role.value());
  }
  return RuntimeUnitExpectation(
      row.runtime_unit_id.value(),
      ToObservationLifecycle(row.lifecycle_control),
      row.product_identity.value(), row.configuration_identity.value(),
      kManagerRuntimeMaximumPresenceAgeNs, kManagerRuntimeMaximumReportAgeNs,
      std::nullopt, std::move(dependencies), std::move(roles),
      ManagedCommandSurfaceState::kClosedNoActiveEffect);
}

void IngestPresence(RuntimeObservationReducer &reducer,
                    const RuntimeUnitConvergenceQueryDTO &query,
                    const ProcessPresenceObservation &sample) {
  if (sample.runtime_unit_id() != query.plan.runtime_unit_id.value() ||
      !reducer.ObserveProcess(sample)) {
    Fail("active process port returned rejected or foreign presence evidence");
  }
}

void IngestEnsuredPresence(RuntimeObservationReducer &reducer,
                           const RuntimeUnitConvergenceQueryDTO &query,
                           const ProcessPresenceObservation &before,
                           const ProcessPresenceObservation &after) {
  if (after.runtime_unit_id() != query.plan.runtime_unit_id.value()) {
    Fail("active process Ensure returned foreign presence evidence");
  }
  const bool accepted =
      after.observer_instance_id() == before.observer_instance_id()
          ? reducer.ObserveProcess(after)
          : reducer.ReplaceProcessObserver(after);
  if (!accepted) {
    Fail("active process Ensure returned rejected presence evidence");
  }
}

[[nodiscard]] ProcessPresenceObservation
ObserveProcess(IRuntimeUnitProcessPort &port,
               const RuntimeUnitConvergenceQueryDTO &query) {
  try {
    return port.ObserveProcess(query);
  } catch (const std::exception &error) {
    Fail("active runtime process observation failed: " +
         std::string(error.what()));
  } catch (...) {
    Fail("active runtime process observation failed");
  }
}

[[nodiscard]] std::optional<RuntimeUnitSourceReport>
ObserveProgress(IRuntimeUnitProgressPort &port,
                const RuntimeUnitConvergenceQueryDTO &query,
                const RuntimeEpoch &runtime_epoch) {
  try {
    return port.ObserveProgress(query, runtime_epoch);
  } catch (const std::exception &error) {
    Fail("active runtime progress observation failed: " +
         std::string(error.what()));
  } catch (...) {
    Fail("active runtime progress observation failed");
  }
}

} // namespace

bool ManagerActiveRuntimePlanDTO::valid() const noexcept {
  if (!active_publication.valid() ||
      runtime_units.size() > kMaximumRuntimeUnits) {
    return false;
  }
  const bool named =
      std::holds_alternative<NamedApplyPublicationTransactionDTO>(
          active_publication.request.transaction_ref);
  if (named != runtime_convergence_intent_digest.has_value() ||
      (!named && !runtime_units.empty())) {
    return false;
  }
  for (std::size_t index = 0U; index < runtime_units.size(); ++index) {
    if (!runtime_units[index].valid() ||
        (index > 0U && !(runtime_units[index - 1U].runtime_unit_id <
                         runtime_units[index].runtime_unit_id))) {
      return false;
    }
    for (const auto &dependency :
         runtime_units[index].dependency_runtime_unit_ids) {
      const auto found = std::lower_bound(
          runtime_units.begin(), runtime_units.end(), dependency,
          [](const auto &row, const auto &runtime_unit_id) {
            return row.runtime_unit_id < runtime_unit_id;
          });
      if (found == runtime_units.end() ||
          found->runtime_unit_id != dependency) {
        return false;
      }
    }
  }
  return true;
}

DurableManagerActiveRuntimePlanSource::DurableManagerActiveRuntimePlanSource(
    const SqliteActiveBundlePublicationStore &publication_store,
    const SqliteManagerApplyJournal &apply_journal)
    : publication_store_(publication_store), apply_journal_(apply_journal) {}

ManagerActiveRuntimePlanDTO
DurableManagerActiveRuntimePlanSource::ReadCurrentPlan() const {
  const auto integrity = publication_store_.VerifyIntegrity();
  if (!integrity.current_generation.nonzero()) {
    Fail("active runtime plan cannot be resolved before bootstrap publication");
  }
  const auto current = publication_store_.ReadActiveBundlePublication(
      integrity.current_generation);
  if (!current.ok() || !current.value().valid()) {
    Fail("active runtime plan could not read the discovered current head");
  }
  const auto &receipt = current.value();
  const auto *transaction = std::get_if<NamedApplyPublicationTransactionDTO>(
      &receipt.request.transaction_ref);
  if (transaction == nullptr) {
    ManagerActiveRuntimePlanDTO empty{receipt, std::nullopt, {}};
    if (!empty.valid()) {
      Fail("bootstrap active runtime plan is invalid");
    }
    return empty;
  }

  const auto durable_publication = apply_journal_.ReadPublicationReceipt(
      transaction->apply_id, transaction->apply_generation);
  const auto runtime_intent = apply_journal_.ReadRuntimeConvergenceIntent(
      transaction->apply_id, transaction->apply_generation);
  if (!durable_publication.has_value() || !runtime_intent.has_value() ||
      durable_publication->publication.publication_receipt != receipt ||
      runtime_intent->runtime_convergence_intent.apply_id !=
          transaction->apply_id ||
      runtime_intent->runtime_convergence_intent.apply_generation !=
          transaction->apply_generation ||
      runtime_intent->runtime_convergence_intent.candidate_retained_bundle_id !=
          receipt.request.candidate_retained_bundle_id ||
      runtime_intent->runtime_convergence_intent.candidate_graph_revision !=
          receipt.request.candidate_graph_revision) {
    Fail("current named publication conflicts with durable runtime evidence");
  }
  ManagerActiveRuntimePlanDTO result{
      receipt,
      runtime_intent->runtime_convergence_intent_digest,
      runtime_intent->runtime_convergence_intent.runtime_units,
  };
  if (!result.valid()) {
    Fail("durable active runtime plan is invalid");
  }
  return result;
}

ManagerActiveRuntimeProcessEffectUncertainError::
    ManagerActiveRuntimeProcessEffectUncertainError(
        std::string detail, const ManagerActiveRuntimeProcessEffect effect,
        RuntimeUnitConvergenceQueryDTO recovery_query)
    : ManagerActiveRuntimeCoordinatorError(std::move(detail)), effect_(effect),
      recovery_query_(std::move(recovery_query)) {}

ManagerActiveRuntimeProcessEffect
ManagerActiveRuntimeProcessEffectUncertainError::effect() const noexcept {
  return effect_;
}

const RuntimeUnitConvergenceQueryDTO &
ManagerActiveRuntimeProcessEffectUncertainError::recovery_query()
    const noexcept {
  return recovery_query_;
}

ManagerActiveRuntimeCoordinator::ManagerActiveRuntimeCoordinator(
    const IManagerActiveRuntimePlanSource &plan_source,
    IRuntimeUnitProcessPort &process_port,
    IRuntimeUnitProgressPort &progress_port,
    RuntimeObservationReducer::Clock clock_ns)
    : plan_source_(plan_source), process_port_(process_port),
      progress_port_(progress_port), clock_ns_(std::move(clock_ns)) {
  if (!clock_ns_) {
    throw std::invalid_argument(
        "active runtime coordinator clock must be callable");
  }
}

ManagerActiveRuntimeConvergenceResult
ManagerActiveRuntimeCoordinator::Reconcile() {
  std::lock_guard operation_lock(operation_mutex_);
  auto plan = plan_source_.ReadCurrentPlan();
  if (!plan.valid()) {
    Fail("active runtime plan source returned an invalid plan");
  }
  const auto order = DependencyOrder(plan.runtime_units);
  std::vector<std::string> declared_ids;
  std::vector<RuntimeUnitExpectation> expectations;
  declared_ids.reserve(plan.runtime_units.size());
  expectations.reserve(plan.runtime_units.size());
  for (const auto &row : plan.runtime_units) {
    declared_ids.push_back(row.runtime_unit_id.value());
    expectations.push_back(MakeExpectation(row));
  }
  if (!observation_plan_.has_value() || *observation_plan_ != plan) {
    observation_reducer_.emplace(clock_ns_, std::move(declared_ids));
    observation_plan_ = plan;
  }
  auto &reducer = *observation_reducer_;

  for (const auto index : order) {
    const auto &row = plan.runtime_units[index];
    const auto query = MakeQuery(plan, row);
    auto process = ObserveProcess(process_port_, query);
    IngestPresence(reducer, query, process);
    if (row.lifecycle_control ==
            manyforge::core::RuntimeLifecycleControl::kManagerManaged &&
        process.state() != ProcessPresenceState::kPresent) {
      try {
        const auto ensured = process_port_.EnsureRunning(query);
        IngestEnsuredPresence(reducer, query, process, ensured);
        process = ensured;
      } catch (const std::exception &error) {
        throw ManagerActiveRuntimeProcessEffectUncertainError(
            "active runtime start outcome is uncertain: " +
                std::string(error.what()),
            ManagerActiveRuntimeProcessEffect::kStart, query);
      } catch (...) {
        throw ManagerActiveRuntimeProcessEffectUncertainError(
            "active runtime start outcome is uncertain",
            ManagerActiveRuntimeProcessEffect::kStart, query);
      }
    }
    if (process.state() == ProcessPresenceState::kPresent) {
      const auto report = ObserveProgress(
          progress_port_, query, RuntimeEpoch(process.process_incarnation()));
      if (report.has_value() && !reducer.IngestSourceReport(*report)) {
        Fail("active progress port returned rejected or foreign source "
             "evidence");
      }
    }
  }

  auto observations = reducer.Evaluate(expectations);
  const bool converged = std::all_of(
      observations.observations().begin(), observations.observations().end(),
      [](const auto &observation) { return observation.converged(); });
  if (plan_source_.ReadCurrentPlan() != plan) {
    Fail("active runtime plan changed during reconciliation");
  }
  return {std::move(plan), std::move(observations),
          converged ? ManagerActiveRuntimeConvergenceState::kConverged
                    : ManagerActiveRuntimeConvergenceState::kPending};
}

ManagerActiveRuntimeShutdownResult ManagerActiveRuntimeCoordinator::Shutdown() {
  std::lock_guard operation_lock(operation_mutex_);
  auto plan = plan_source_.ReadCurrentPlan();
  if (!plan.valid()) {
    Fail("active runtime plan source returned an invalid plan");
  }
  auto order = DependencyOrder(plan.runtime_units);
  std::reverse(order.begin(), order.end());
  std::vector<ProcessPresenceObservation> final_observations;
  final_observations.reserve(order.size());
  for (const auto index : order) {
    const auto &row = plan.runtime_units[index];
    const auto query = MakeQuery(plan, row);
    auto process = ObserveProcess(process_port_, query);
    if (row.lifecycle_control ==
            manyforge::core::RuntimeLifecycleControl::kManagerManaged &&
        process.state() != ProcessPresenceState::kAbsent) {
      try {
        process = process_port_.EnsureStopped(query);
      } catch (const std::exception &error) {
        throw ManagerActiveRuntimeProcessEffectUncertainError(
            "active runtime stop outcome is uncertain: " +
                std::string(error.what()),
            ManagerActiveRuntimeProcessEffect::kStop, query);
      } catch (...) {
        throw ManagerActiveRuntimeProcessEffectUncertainError(
            "active runtime stop outcome is uncertain",
            ManagerActiveRuntimeProcessEffect::kStop, query);
      }
      if (process.runtime_unit_id() != row.runtime_unit_id.value() ||
          process.state() != ProcessPresenceState::kAbsent) {
        throw ManagerActiveRuntimeProcessEffectUncertainError(
            "active runtime stop did not prove exact process absence",
            ManagerActiveRuntimeProcessEffect::kStop, query);
      }
    }
    final_observations.push_back(std::move(process));
  }
  if (plan_source_.ReadCurrentPlan() != plan) {
    Fail("active runtime plan changed during orderly shutdown");
  }
  return {std::move(plan), std::move(final_observations)};
}

} // namespace manyforge::component_manager
