#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

// This is logical registration shape only. Actual CCLC/provider currentness
// remains the Kernel's independent command-admission proof.
bool IsLogicalOutputRegistration(
    const core::RuntimeParticipantExecutionRegistration &registration) {
  const auto &binding = registration.binding.value();
  return registration.engine != nullptr && binding.io_output_binding &&
         binding.io_output_binding->signal.output_resource &&
         binding.resources.empty() && !binding.control_path &&
         !registration.stop_readiness &&
         registration.stop_execution_port == nullptr;
}

void ValidateRegistrations(
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations,
    const core::RuntimeParticipantAdmissionResolver &resolver) {
  if (!registrations.empty() && !resolver) {
    throw std::invalid_argument(
        "native cell participants require an exact Kernel resolver");
  }
  for (std::size_t index = 0U; index < registrations.size(); ++index) {
    const auto &registration = registrations[index];
    if (registration.engine == nullptr ||
        (registration.binding.value().io_output_binding
             ? !IsLogicalOutputRegistration(registration)
             : registration.binding.value().resources.empty())) {
      throw std::invalid_argument(
          "native cell registration has no engine or exact command resources");
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (registrations[previous].binding.value() ==
          registration.binding.value()) {
        throw std::invalid_argument(
            "native cell registration set repeats an exact binding");
      }
    }
  }
}

} // namespace

core::Result<void> RetainedNativeCellRuntime::ValidateStopSourceCoverage(
    const std::vector<RetainedStopSurveillanceSourceInput> &sources,
    const RetainedStopSurveillanceEmptySourceMode empty_source_mode,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations) {
  const auto fail = [](std::string reason, std::string message,
                       const core::RuntimeParticipantBinding *binding =
                           nullptr) {
    core::ErrorDTO error{"native_cell_stop_sources_invalid",
                         std::move(message),
                         {{"reason", std::move(reason)}}};
    if (binding != nullptr) {
      error.details.emplace("controllable_component_id",
                            binding->value().controllable_component_id.value());
      error.details.emplace("command_target_id",
                            binding->value().command_target_id.value());
      if (binding->value().control_path.has_value())
        error.details.emplace(
            "control_binding_id",
            binding->value().control_path->control_binding_id.value());
    }
    return core::Result<void>::Err(std::move(error));
  };
  if (sources.empty() &&
      empty_source_mode ==
          RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty) {
    if (std::all_of(registrations.begin(), registrations.end(),
                    IsLogicalOutputRegistration)) {
      return core::Result<void>::Ok();
    }
    return fail("native_cell_stop_sources_missing",
                "native cell with physical participants requires Stop sources");
  }
  const auto valid = RetainedStopSurveillanceOwner::ValidateSources(sources);
  if (!valid.ok()) {
    return valid;
  }
  std::vector<const core::RuntimeParticipantExecutionRegistration *>
      source_registrations;
  source_registrations.reserve(sources.size());
  for (const auto &source : sources) {
    const auto matches = [&](const auto &candidate) {
      return candidate.binding.value() == source.binding.value();
    };
    const auto registration =
        std::find_if(registrations.begin(), registrations.end(), matches);
    if (registration == registrations.end() ||
        registration->engine == nullptr ||
        std::count_if(registrations.begin(), registrations.end(), matches) !=
            1) {
      return fail("native_cell_stop_source_registration_not_unique",
                  "native cell Stop source is not one exact registration",
                  &source.binding);
    }
    source_registrations.push_back(&*registration);
  }
  for (const auto &registration : registrations) {
    if (!registration.stop_readiness.has_value()) {
      continue;
    }
    const auto source = std::find_if(
        sources.begin(), sources.end(), [&](const auto &candidate) {
          const auto &physical = candidate.binding.value();
          const auto &covered = registration.binding.value();
          return core::runtime_participant_binding_physically_contains(
                     candidate.binding, registration.binding) ||
                 (physical.control_path.has_value() &&
                  covered.control_path.has_value() &&
                  physical.program_id == covered.program_id &&
                  physical.program_revision == covered.program_revision &&
                  physical.program_binding_id == covered.program_binding_id &&
                  physical.component_role_id == covered.component_role_id &&
                  physical.target_role_id == covered.target_role_id &&
                  physical.control_path->control_binding_id !=
                      covered.control_path->control_binding_id &&
                  retained_bindings_share_backend_owner(candidate.binding,
                                                        registration.binding));
        });
    if (source == sources.end()) {
      return fail(
          "native_cell_stop_source_coverage_missing",
          "native cell Stop source table omits a Stop-qualified participant",
          &registration.binding);
    }
    const auto &physical = *source_registrations[static_cast<std::size_t>(
        std::distance(sources.begin(), source))];
    if (!physical.stop_readiness.has_value() ||
        physical.engine != registration.engine ||
        physical.stop_execution_port == nullptr ||
        physical.stop_execution_port != registration.stop_execution_port ||
        physical.stop_readiness->value().controllable_component_id !=
            registration.stop_readiness->value().controllable_component_id ||
        physical.stop_readiness->value().component_lifecycle_generation !=
            registration.stop_readiness->value()
                .component_lifecycle_generation ||
        physical.stop_readiness->value().expected_controller_domain !=
            registration.stop_readiness->value().expected_controller_domain) {
      return fail("native_cell_stop_source_owner_mismatch",
                  "native cell Stop source changes its covered engine, Stop "
                  "port, or authority",
                  &registration.binding);
    }
    for (const auto &stage : registration.stop_readiness->value()
                                 .resolved_stop_base_plan.escalation_stages) {
      auto resources = std::vector<core::ResourceId>{};
      if (const auto *scope =
              std::get_if<core::ResolvedStopResourceContainmentScopeDTO>(
                  &stage.containment_scope)) {
        resources = scope->resource_ids;
      } else {
        for (const auto &resource : registration.binding.value().resources)
          resources.push_back(resource.resource_id);
        std::sort(resources.begin(), resources.end());
      }
      for (const auto &requirement : stage.evidence_requirements) {
        if (requirement.requirement.evidence_kind !=
                core::StopEvidenceRequirementKind::kMotionRest ||
            requirement.resolved_source.source_kind !=
                core::StopEvidenceSourceKind::kBindingStateAuthority)
          continue;
        const RetainedStopSurveillanceObservationSource expected{
            requirement.requirement, requirement.resolved_source, resources};
        if (std::find(source->observation_sources.begin(),
                      source->observation_sources.end(),
                      expected) == source->observation_sources.end()) {
          return fail("native_cell_stop_source_evidence_missing",
                      "native cell Stop source omits an exact covered evidence "
                      "requirement",
                      &registration.binding);
        }
      }
    }
  }
  return core::Result<void>::Ok();
}

RetainedNativeCellRuntime::RetainedNativeCellRuntime(
    RetainedNativeCellRuntimeInput input)
    : clock_(std::move(input.clock)),
      scheduler_(std::move(input.scheduler)),
      observation_wake_(std::make_shared<RetainedObservationWake>()),
      stop_source_mode_(input.stop_source_mode),
      request_owner_epoch_(input.kernel_config.request_owner_epoch),
      cell_id_(input.kernel_config.cell_id),
      operation_admission_gate_(
          input.kernel_config.operation_admission_gate),
      participant_registrations_(std::move(input.participant_registrations)),
      participant_resolver_(input.kernel_config.participant_resolver) {
  if (clock_ == nullptr || scheduler_ == nullptr ||
      input.operation_journal_path.empty() ||
      input.kernel_config.request_owner_epoch.empty() ||
      input.kernel_config.cell_id.empty() ||
      input.kernel_config.stop_surveillance.has_value()) {
    throw std::invalid_argument(
        "native cell owner has incomplete or externally owned Kernel inputs");
  }
  if (participant_registrations_.empty()) {
    if (!input.kernel_config.empty_runtime_graph_revision.has_value() ||
        input.kernel_config.empty_runtime_graph_revision->empty() ||
        input.kernel_config.empty_runtime_retained_bundle_id.has_value() !=
            input.kernel_config.empty_runtime_graph_revision.has_value()) {
      throw std::invalid_argument(
          "empty native cell owner requires one exact empty-product graph");
    }
  }
  ValidateRegistrations(participant_registrations_, participant_resolver_);
  const auto stop_sources = ValidateStopSourceCoverage(
      input.stop_sources, input.stop_source_mode, participant_registrations_);
  if (!stop_sources.ok())
    throw std::invalid_argument(stop_sources.error().message);

  const auto observer_clock = clock_;
  stop_surveillance_owner_ = std::shared_ptr<RetainedStopSurveillanceOwner>(
      new RetainedStopSurveillanceOwner(std::move(input.stop_sources),
                                        clock_.get(), input.stop_source_mode),
      [observer_clock](RetainedStopSurveillanceOwner *observer) {
        delete observer;
        // The observer stores a non-owning IClock pointer. Any shared owner
        // handle must keep that clock alive through observer destruction.
        (void)observer_clock;
      });
  operation_journal_ = std::make_unique<core::SqliteOperationJournal>(
      std::move(input.operation_journal_path), input.kernel_config.cell_id);

  input.kernel_config.participant_resolver = participant_resolver_;
  if (input.kernel_config.prequalified_max_live_stop_participants == 0U) {
    input.kernel_config.prequalified_max_live_stop_participants =
        kNativeMaximumLiveStopParticipants;
  }
  input.kernel_config.stop_surveillance =
      core::StopSurveillanceExecutionConfig{
          stop_surveillance_owner_.get(), input.stop_surveillance_policy};
  if (!input.kernel_config.stop_fault_recovery_facts_resolver) {
    input.kernel_config.stop_fault_recovery_facts_resolver =
        [this](const core::StopStatusDTO &status,
               const std::int64_t assessed_at_ns) {
          return BuildStopRecoveryFacts(status, assessed_at_ns);
        };
  }

  addressed_kernel_ =
      std::make_unique<core::ResourceAddressedExecutionKernel>(
          participant_registrations_, *clock_, *scheduler_,
          *operation_journal_, std::move(input.kernel_config));
  // A callback that throws on this scheduler's Stop/safety path, including a
  // lookahead session of an engine sharing it, contains the cell.
  scheduler_->set_safety_path_fault_sink(
      [kernel = addressed_kernel_.get()](const std::string_view reason_code) {
        kernel->ContainInternalFault(reason_code);
      });
}

RetainedNativeCellRuntime::~RetainedNativeCellRuntime() {
  // The sink borrows the Kernel; the shared scheduler may outlive both.
  scheduler_->set_safety_path_fault_sink({});
  // The Kernel owns non-owning subscriptions to every registered engine and
  // references the journal, observer, clock, and scheduler. Retire it while
  // every referenced owner remains alive.
  addressed_kernel_.reset();
  operation_journal_.reset();
  stop_surveillance_owner_.reset();
  participant_resolver_ = {};
  participant_registrations_.clear();
  scheduler_.reset();
  clock_.reset();
}

std::shared_ptr<WallClock> RetainedNativeCellRuntime::clock() const noexcept {
  return clock_;
}

std::shared_ptr<SimpleScheduler>
RetainedNativeCellRuntime::scheduler() const noexcept {
  return scheduler_;
}

std::shared_ptr<RetainedObservationWake>
RetainedNativeCellRuntime::observation_wake() const noexcept {
  return observation_wake_;
}

std::shared_ptr<RetainedStopSurveillanceOwner>
RetainedNativeCellRuntime::stop_surveillance_owner() const noexcept {
  return stop_surveillance_owner_;
}

core::SqliteOperationJournal &
RetainedNativeCellRuntime::operation_journal() noexcept {
  return *operation_journal_;
}

const core::SqliteOperationJournal &
RetainedNativeCellRuntime::operation_journal() const noexcept {
  return *operation_journal_;
}

core::ResourceAddressedExecutionKernel &
RetainedNativeCellRuntime::addressed_kernel() noexcept {
  return *addressed_kernel_;
}

const core::ResourceAddressedExecutionKernel &
RetainedNativeCellRuntime::addressed_kernel() const noexcept {
  return *addressed_kernel_;
}

const std::vector<core::RuntimeParticipantExecutionRegistration> &
RetainedNativeCellRuntime::participant_registrations() const noexcept {
  return participant_registrations_;
}

const core::RuntimeParticipantAdmissionResolver &
RetainedNativeCellRuntime::participant_resolver() const noexcept {
  return participant_resolver_;
}

core::Result<core::StopFaultRecoveryFactsDTO>
RetainedNativeCellRuntime::BuildStopRecoveryFacts(
    const core::StopStatusDTO &status, const std::int64_t assessed_at_ns) {
  if (stop_surveillance_owner_ == nullptr || addressed_kernel_ == nullptr ||
      clock_ == nullptr) {
    return core::Result<core::StopFaultRecoveryFactsDTO>::Err(
        {std::string(core::to_string(core::ErrorCode::kMissingReference)),
         "native cell Stop recovery owner is unavailable",
         {{"reason", "native_stop_recovery_owner_unavailable"}}});
  }
  return build_retained_cell_stop_recovery_facts(
      *stop_surveillance_owner_, *addressed_kernel_, *clock_,
      request_owner_epoch_, cell_id_, operation_admission_gate_, status,
      assessed_at_ns);
}

core::Result<void> RetainedNativeCellRuntime::ReconcileCommandFreeOrphans() {
  if (stop_surveillance_owner_ == nullptr || addressed_kernel_ == nullptr ||
      clock_ == nullptr) {
    return core::Result<void>::Err(
        {std::string(core::to_string(core::ErrorCode::kMissingReference)),
         "native cell orphan reconciliation owner is unavailable",
         {{"reason", "native_orphan_reconciliation_owner_missing"}}});
  }
  const auto slots = addressed_kernel_->resource_table_snapshot();
  std::vector<core::ResourceId> resources;
  for (const auto &slot : slots.slots) {
    if (slot.state ==
        core::ResourceSlotState::kUnreconciledExternalOrOrphanGoal)
      resources.push_back(slot.resource_id);
  }
  if (resources.empty())
    return core::Result<void>::Ok();
  auto facts =
      stop_surveillance_owner_->BuildOrphanResourceReconciliationFacts(
          request_owner_epoch_, cell_id_,
          slots.operation_resource_generation, resources, clock_->now());
  if (!facts.ok())
    return core::Result<void>::Err(facts.error());
  auto reconciled = addressed_kernel_->reconcile_orphaned_resources(facts.value());
  if (!reconciled.ok())
    return core::Result<void>::Err(reconciled.error());
  return core::Result<void>::Ok();
}

void RetainedNativeCellRuntime::AdoptVerifiedComponentReadinessSnapshot(
    std::vector<core::RuntimeParticipantExecutionRegistration> &&registrations,
    core::RuntimeParticipantAdmissionResolver &&participant_resolver) noexcept {
  // Both swaps are allocation-free and noexcept. The caller has already
  // verified this exact candidate against the live Kernel while holding the
  // transition authority documented in the public contract.
  participant_registrations_.swap(registrations);
  participant_resolver_.swap(participant_resolver);
}

core::Result<void> RetainedNativeCellRuntime::reconfigure_runtime_product(
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations,
    core::RuntimeParticipantAdmissionResolver participant_resolver,
    std::vector<RetainedStopSurveillanceSourceInput> candidate_stop_sources,
    std::function<core::Result<void>()> commit_publication) {
  try {
    ValidateRegistrations(registrations, participant_resolver);
  } catch (const std::exception &exception) {
    return core::Result<void>::Err(
        {"native_cell_registration_invalid", exception.what(), {}});
  }
  const auto stop_sources = ValidateStopSourceCoverage(
      candidate_stop_sources, stop_source_mode_, registrations);
  if (!stop_sources.ok()) return stop_sources;
  const auto observer = stop_surveillance_owner_;
  auto publish = [observer,
                  sources = std::move(candidate_stop_sources),
                  commit_publication = std::move(commit_publication)]() mutable {
    // Keep the previously published Stop view if a separate publication
    // callback refuses. Kernel admission remains closed on any callback
    // failure; publishing candidate Stop sources first would pair the old
    // Kernel registrations with a new observation table during that failure.
    if (commit_publication) {
      auto committed = commit_publication();
      if (!committed.ok()) {
        return committed;
      }
    }
    return observer->ReplaceSources(std::move(sources));
  };
  auto result = addressed_kernel_->reconfigure_runtime_product(
      registrations, participant_resolver, std::move(publish));
  if (!result.ok()) {
    return result;
  }
  participant_registrations_ = std::move(registrations);
  participant_resolver_ = std::move(participant_resolver);
  return core::Result<void>::Ok();
}

core::Result<void> RetainedNativeCellRuntime::reconfigure_runtime_component_set(
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations,
    core::RuntimeParticipantAdmissionResolver participant_resolver,
    std::vector<RetainedStopSurveillanceSourceInput> candidate_stop_sources,
    core::RetainedBundleId candidate_retained_bundle_id,
    core::GraphRevision candidate_graph_revision,
    const core::ConfigurationFenceId &apply_fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::vector<core::ControllableComponentId> &retained_components,
    std::function<core::Result<void>()> commit_publication,
    std::span<const core::ResolvedLifecycleControlDomainScopeDTO>
        retained_provider_domains,
    std::span<const core::ResolvedLifecycleProviderOccurrenceDTO>
        retained_provider_occurrences,
    const core::CellAdmissionClaimDTO *actual_parent_claim) {
  try {
    ValidateRegistrations(registrations, participant_resolver);
  } catch (const std::exception &exception) {
    return core::Result<void>::Err(
        {"native_cell_registration_invalid", exception.what(), {}});
  }
  const auto stop_sources = ValidateStopSourceCoverage(
      candidate_stop_sources, stop_source_mode_, registrations);
  if (!stop_sources.ok()) return stop_sources;
  const auto observer = stop_surveillance_owner_;
  auto publish = [observer,
                  sources = std::move(candidate_stop_sources),
                  commit_publication = std::move(commit_publication)]() mutable {
    // Keep the previously published Stop view if a separate publication
    // callback refuses. Kernel admission remains closed on any callback
    // failure; publishing candidate Stop sources first would pair the old
    // Kernel registrations with a new observation table during that failure.
    if (commit_publication) {
      auto committed = commit_publication();
      if (!committed.ok()) {
        return committed;
      }
    }
    return observer->ReplaceSources(std::move(sources));
  };
  auto result = addressed_kernel_->reconfigure_runtime_component_set(
      registrations, participant_resolver,
      std::move(candidate_retained_bundle_id),
      std::move(candidate_graph_revision), apply_fence_id, scope,
      retained_components, std::move(publish), retained_provider_domains,
      retained_provider_occurrences, actual_parent_claim);
  if (!result.ok()) {
    return result;
  }
  participant_registrations_ = std::move(registrations);
  participant_resolver_ = std::move(participant_resolver);
  return core::Result<void>::Ok();
}

}  // namespace manyforge::roboplan_backend
