#include <manyforge_roboplan/retained_cell_runtime.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

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
        registration.binding.value().resources.empty()) {
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

RetainedCellRuntime::RetainedCellRuntime(
    RetainedCellRuntimeInput input)
    : clock_(std::move(input.clock)),
      scheduler_(std::move(input.scheduler)),
      observation_wake_(std::make_shared<RetainedObservationWake>()),
      participant_registrations_(std::move(input.participant_registrations)),
      participant_resolver_(input.kernel_config.participant_resolver) {
  if (clock_ == nullptr || scheduler_ == nullptr ||
      input.operation_journal_path.empty() ||
      input.kernel_config.request_owner_epoch.empty() ||
      input.kernel_config.cell_id.empty()) {
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
  operation_journal_ = std::make_unique<core::SqliteOperationJournal>(
      std::move(input.operation_journal_path), input.kernel_config.cell_id);

  input.kernel_config.participant_resolver = participant_resolver_;
  if (input.kernel_config.prequalified_max_live_stop_participants == 0U) {
    input.kernel_config.prequalified_max_live_stop_participants =
        kNativeMaximumLiveStopParticipants;
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

RetainedCellRuntime::~RetainedCellRuntime() {
  // The sink borrows the Kernel; the shared scheduler may outlive both.
  scheduler_->set_safety_path_fault_sink({});
  // The Kernel owns non-owning subscriptions to every registered engine and
  // references the journal, clock, and scheduler. Retire it while every
  // referenced owner remains alive.
  addressed_kernel_.reset();
  operation_journal_.reset();
  participant_resolver_ = {};
  participant_registrations_.clear();
  scheduler_.reset();
  clock_.reset();
}

std::shared_ptr<WallClock> RetainedCellRuntime::clock() const noexcept {
  return clock_;
}

std::shared_ptr<SimpleScheduler>
RetainedCellRuntime::scheduler() const noexcept {
  return scheduler_;
}

std::shared_ptr<RetainedObservationWake>
RetainedCellRuntime::observation_wake() const noexcept {
  return observation_wake_;
}

core::SqliteOperationJournal &
RetainedCellRuntime::operation_journal() noexcept {
  return *operation_journal_;
}

const core::SqliteOperationJournal &
RetainedCellRuntime::operation_journal() const noexcept {
  return *operation_journal_;
}

core::ResourceAddressedExecutionKernel &
RetainedCellRuntime::addressed_kernel() noexcept {
  return *addressed_kernel_;
}

const core::ResourceAddressedExecutionKernel &
RetainedCellRuntime::addressed_kernel() const noexcept {
  return *addressed_kernel_;
}

const std::vector<core::RuntimeParticipantExecutionRegistration> &
RetainedCellRuntime::participant_registrations() const noexcept {
  return participant_registrations_;
}

const core::RuntimeParticipantAdmissionResolver &
RetainedCellRuntime::participant_resolver() const noexcept {
  return participant_resolver_;
}

void RetainedCellRuntime::AdoptVerifiedComponentReadinessSnapshot(
    std::vector<core::RuntimeParticipantExecutionRegistration> &&registrations,
    core::RuntimeParticipantAdmissionResolver &&participant_resolver) noexcept {
  // Both swaps are allocation-free and noexcept. The caller has already
  // verified this exact candidate against the live Kernel while holding the
  // transition authority documented in the public contract.
  participant_registrations_.swap(registrations);
  participant_resolver_.swap(participant_resolver);
}

core::Result<void> RetainedCellRuntime::reconfigure_runtime_product(
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations,
    core::RuntimeParticipantAdmissionResolver participant_resolver,
    std::function<core::Result<void>()> commit_publication) {
  try {
    ValidateRegistrations(registrations, participant_resolver);
  } catch (const std::exception &exception) {
    return core::Result<void>::Err(
        {"native_cell_registration_invalid", exception.what(), {}});
  }
  auto result = addressed_kernel_->reconfigure_runtime_product(
      registrations, participant_resolver, std::move(commit_publication));
  if (!result.ok()) {
    return result;
  }
  participant_registrations_ = std::move(registrations);
  participant_resolver_ = std::move(participant_resolver);
  return core::Result<void>::Ok();
}

core::Result<void> RetainedCellRuntime::reconfigure_runtime_component_set(
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations,
    core::RuntimeParticipantAdmissionResolver participant_resolver,
    core::RetainedBundleId candidate_retained_bundle_id,
    core::GraphRevision candidate_graph_revision,
    const core::ConfigurationFenceId &apply_fence_id,
    const core::ConfigurationFenceScopeDTO &scope,
    const std::vector<core::ControllableComponentId> &retained_components,
    std::function<core::Result<void>()> commit_publication) {
  try {
    ValidateRegistrations(registrations, participant_resolver);
  } catch (const std::exception &exception) {
    return core::Result<void>::Err(
        {"native_cell_registration_invalid", exception.what(), {}});
  }
  auto result = addressed_kernel_->reconfigure_runtime_component_set(
      registrations, participant_resolver,
      std::move(candidate_retained_bundle_id),
      std::move(candidate_graph_revision), apply_fence_id, scope,
      retained_components, std::move(commit_publication));
  if (!result.ok()) {
    return result;
  }
  participant_registrations_ = std::move(registrations);
  participant_resolver_ = std::move(participant_resolver);
  return core::Result<void>::Ok();
}

}  // namespace manyforge::roboplan_backend
