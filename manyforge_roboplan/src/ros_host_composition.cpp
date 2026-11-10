#include "ros_host_composition.hpp"

#include "execution_host_posix.hpp"
#include "host_io_signal_runtime.hpp"
#include "io_provider_lifecycle_product.hpp"

#include <manyforge_roboplan/controller_launch_process.hpp>
#include <manyforge_roboplan/adapter_transport_consumer.hpp>
#include <manyforge_roboplan/retained_cell_runtime.hpp>
#include <manyforge_roboplan/retained_component_owner.hpp>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_core/types/typed_error.hpp>

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

core::Result<std::shared_ptr<void>> EnterInstalledTransportEffect(
    ExecutionCompositionAccess &access, const std::string &component_id,
    const std::shared_ptr<const InstalledGenericIoMembership> &generic,
    const std::shared_ptr<const InstalledAdapterTransportMembership> &adapter,
    const std::shared_ptr<const InstalledRetainedTransportMembership> &retained) {
  using Reply = core::Result<std::shared_ptr<void>>;
  const bool uses_generic = generic &&
      std::any_of(generic->components.begin(), generic->components.end(),
                  [&](const auto &id) { return id.value() == component_id; });
  const bool uses_adapter = adapter &&
      std::any_of(adapter->consumers.begin(), adapter->consumers.end(),
                  [&](const auto &entry) { return entry.first == component_id; });
  const bool uses_retained = retained &&
      std::any_of(retained->entries.begin(), retained->entries.end(),
                  [&](const auto &entry) { return entry.component == component_id; });
  if (!uses_generic && !uses_adapter && !uses_retained) return Reply::Ok(nullptr);
  auto permit = access.TryTransportEffect();
  if (!permit)
    return Reply::Err({"component_action_in_progress",
                       "A shared transport recovery is in progress",
                       {{"component_id", component_id},
                        {"effect_disposition", "known_no_effect"}}});
  return Reply::Ok(std::make_shared<ExecutionCompositionAccess::TransportAdmission>(
      std::move(*permit)));
}

core::Result<void> ResetCompositionActiveTcps(
    Composition &composition, ExecutionCompositionAccess &access) {
  using Reply = core::Result<void>;
  std::vector<std::pair<const Composition *, std::shared_ptr<void>>> members;
  const auto admit = [&](const Composition &view) -> Reply {
    if (!view.ros2_control_runtime_handle) return Reply::Ok();
    std::shared_ptr<void> permit;
    if (access.enter_transport_effect) {
      auto acquired = access.enter_transport_effect(view.controllable_component_id.value());
      if (!acquired.ok()) return Reply::Err(acquired.error());
      permit = std::move(acquired.value());
    }
    members.emplace_back(&view, std::move(permit));
    return Reply::Ok();
  };
  auto result = admit(composition);
  if (!result.ok()) return result;
  for (const auto &participant : composition.participant_compositions) {
    result = admit(*participant);
    if (!result.ok()) return result;
  }
  for (const auto &[view, permit] : members) {
    result = permit ? view->ros2_control_runtime_handle->ResetActiveTcpAdmitted()
                    : view->ros2_control_runtime_handle->ResetActiveTcp();
    if (!result.ok()) return result;
  }
  return Reply::Ok();
}

core::Result<std::shared_ptr<StagedAdapterTransportRecovery>>
StageAdapterTransportRecovery(
    std::span<const std::pair<std::string, std::shared_ptr<AdapterTransportConsumer>>> consumers,
    const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &failed,
    const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &replacement) {
  using Reply = core::Result<std::shared_ptr<StagedAdapterTransportRecovery>>;
  if (!failed || !replacement || failed == replacement)
    return Reply::Err({"component_not_ready", "The transport recovery owner is invalid", {}});
  if (!failed->joined())
    return Reply::Err({"component_not_ready", "The failed transport has not joined", {}});
  auto result = std::make_shared<StagedAdapterTransportRecovery>();
  result->active = std::make_shared<std::atomic<bool>>(false);
  result->failed = failed;
  result->replacement = replacement;
  std::set<const AdapterTransportConsumer *> included;
  try {
    for (const auto &[component_id, consumer] : consumers) {
      static_cast<void>(component_id);
      if (!consumer || !included.insert(consumer.get()).second) continue;
      auto stage = consumer->Stage(failed, replacement, result->active);
      if (!stage)
        return Reply::Err({"component_not_ready", "An adapter transport could not be staged", {}});
      result->entries.push_back({consumer, std::move(stage)});
    }
  } catch (const core::TypedError &error) {
    return Reply::Err(error.error());
  } catch (const std::exception &error) {
    return Reply::Err({"component_not_ready", error.what(), {}});
  }
  return Reply::Ok(std::move(result));
}

core::Result<void> CommitAdapterTransportRecovery(StagedAdapterTransportRecovery &stage) {
  using Reply = core::Result<void>;
  if (stage.committed || !stage.failed->joined() || !stage.replacement->started() ||
      stage.replacement->executor_fault())
    return Reply::Err({"component_not_ready", "The replacement transport is not ready", {}});
  try {
    for (const auto &entry : stage.entries)
      if (!entry.consumer->Validate(entry.stage))
        return Reply::Err({"component_not_ready", "An adapter changed during transport recovery", {}});
  } catch (const core::TypedError &error) {
    return Reply::Err(error.error());
  } catch (const std::exception &error) {
    return Reply::Err({"component_not_ready", error.what(), {}});
  }
  for (const auto &entry : stage.entries) entry.consumer->Commit(entry.stage);
  stage.committed = true;
  return Reply::Ok();
}

core::Result<void> ActivateAdapterTransportRecovery(
    StagedAdapterTransportRecovery &stage,
    const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &published_owner) {
  if (!stage.committed || published_owner != stage.replacement ||
      !stage.replacement->started() || stage.replacement->executor_fault())
    return core::Result<void>::Err(
        {"component_not_ready", "The replacement transport is not published and healthy", {}});
  stage.active->store(true, std::memory_order_release);
  return core::Result<void>::Ok();
}

bool IsProviderOnlyNativeComponent(
    const core::CellGraphDTO &qualified_graph,
    const core::ControllableComponentId &component_id) {
  const auto &components = qualified_graph.controllable_components;
  const auto component =
      std::find_if(components.begin(), components.end(), [&](const auto &row) {
        return row.controllable_component_id == component_id;
      });
  if (component == components.end() ||
      std::count_if(components.begin(), components.end(),
                    [&](const auto &row) {
                      return row.controllable_component_id == component_id;
                    }) != 1 ||
      std::any_of(qualified_graph.control_bindings.begin(),
                  qualified_graph.control_bindings.end(), [&](const auto &row) {
                    return row.controllable_component_id == component_id;
                  }))
    return false;
  return std::any_of(
      qualified_graph.control_domains.begin(),
      qualified_graph.control_domains.end(), [&](const auto &domain) {
        const auto *provider = domain.provider();
        return provider &&
               provider->controllable_component_id == component_id &&
               provider->locally_valid() &&
               std::find(component->runtime_unit_ids.begin(),
                         component->runtime_unit_ids.end(),
                         provider->provider_runtime_unit_id) !=
                   component->runtime_unit_ids.end() &&
               std::find(domain.runtime_unit_ids.begin(),
                         domain.runtime_unit_ids.end(),
                         provider->provider_runtime_unit_id) !=
                   domain.runtime_unit_ids.end();
      });
}

std::vector<core::ControllableComponentId> MemberCarriedOwnerIds(const Composition &cell) {
  std::vector<const Composition *> views{&cell};
  for (const auto &peer : cell.participant_compositions) views.push_back(peer.get());
  std::vector<core::ControllableComponentId> owners;
  for (const auto *view : views)
    if (view->owner_component &&
        view->owner_component->component_id != view->controllable_component_id.value() &&
        std::none_of(views.begin(), views.end(), [&](const auto *other) {
          return other->controllable_component_id.value() == view->owner_component->component_id;
        }))
      owners.emplace_back(view->owner_component->component_id);
  std::sort(owners.begin(), owners.end());
  owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
  return owners;
}

core::Result<MockIoInputSetResult> SetCompositionMockInput(
    Composition &composition, const core::IoSignalId &signal,
    const core::IoSourceIncarnation &expected_source_epoch, bool value) {
  if (!composition.io_signal_runtime ||
      composition.io_signal_runtime->service() != composition.io_signal_service)
    return core::Result<MockIoInputSetResult>::Err(
        {"io_signal_service_unavailable", "Current Mock acquisition owner is absent", {}});
  return composition.io_signal_runtime->SetMockInput(signal, expected_source_epoch, value);
}

ExecutionProduct::ExecutionProduct(
    std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
    std::shared_ptr<const deployment::ProgramBindingContract> selected_program,
    std::shared_ptr<const deployment::ProgramBinding> selected_binding,
    core::ProjectionRevisionDTO selected_projection,
    core::ActiveBundlePublicationGeneration selected_generation)
    : bundle(std::move(selected_bundle)), program(std::move(selected_program)),
      program_binding(std::move(selected_binding)),
      projection_revision(std::move(selected_projection)), publication_generation(selected_generation) {}

// A logical predecessor's active/staged service is also owned by the Kernel.
// Let that actual service lifetime retire its runtime after commit/discard.
// Physical Composition preserves its explicit shutdown ordering below.
ExecutionProduct::~ExecutionProduct() = default;

void Composition::StageRobotTcpReader() {
  staged_robot_tcp_reader =
      [owner = std::weak_ptr(ros2_control_runtime_handle)]() -> std::optional<ActiveTcp> {
        const auto handle = owner.lock();
        if (!handle) return std::nullopt;
        return handle->ReadRobotTcps();
      };
}

Composition::~Composition() {
    if (io_signal_runtime) io_signal_runtime->Shutdown();
    if (prepared_controller_launch && !controller_launch_adopted)
      static_cast<void>(host_controller_launch_supervisor().discard_candidate(
          *prepared_controller_launch));
  }

// A failed fence keeps engine/runtime pins, but its uninstalled process
  // has no active owner. Borrowed and already-adopted launches remain owned.
  void Composition::discard_unadopted_controller_launches() {
    if (prepared_controller_launch && !controller_launch_adopted)
      static_cast<void>(host_controller_launch_supervisor().discard_candidate(
          *prepared_controller_launch));
    for (auto &peer : participant_compositions)
      peer->discard_unadopted_controller_launches();
  }

[[nodiscard]] core::ResourceAddressedExecutionKernel *
  Composition::active_addressed_kernel() const noexcept {
    if (cell_runtime != nullptr) {
      return &cell_runtime->addressed_kernel();
    }
    return runtime != nullptr ? runtime->cell_addressed_kernel : nullptr;
  }

[[nodiscard]] const void *Composition::component_runtime_identity() const noexcept {
  if (component_runtime_pins)
    return component_runtime_pins.get();
  if (runtime)
    return runtime.get();
  if (gripper_runtime)
    return gripper_runtime.get();
  if (io_provider_drivers && !io_provider_drivers->empty())
    return io_provider_drivers.get();
  // An adapter-only component (e.g. a sensor) is its adapter.
  return component_adapter.get();
  }

[[nodiscard]] std::vector<core::RuntimeUnitEpochBindingDTO>
  Composition::host_runtime_unit_epochs() const {
  std::vector<core::RuntimeUnitEpochBindingDTO> epochs;
  if (runtime != nullptr)
    epochs = runtime->motion_binding.value().runtime_unit_epochs;
  else if (gripper_runtime != nullptr)
    // binding() returns a value; copy its epoch list so callers never retain
    // a reference into that temporary.
    epochs = gripper_runtime->binding().value().runtime_unit_epochs;
  if (io_provider_drivers && !io_provider_drivers->empty()) {
    for (const auto &driver : *io_provider_drivers) {
      Require(driver != nullptr, "I/O component lost its actual driver");
      epochs.push_back(driver->runtime_unit_epoch());
    }
    const auto less = [](const auto &a, const auto &b) {
      return std::tie(a.runtime_unit_id, a.runtime_epoch) <
             std::tie(b.runtime_unit_id, b.runtime_epoch);
    };
    std::sort(epochs.begin(), epochs.end(), less);
    epochs.erase(std::unique(epochs.begin(), epochs.end()), epochs.end());
  }
  Require(!epochs.empty(),
          "native component has no runtime-unit identity source");
  return epochs;
  }

[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
  Composition::component_registrations() const {
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  if (runtime != nullptr)
    result = RetainedComponentOwner(runtime).registrations();
  else if (gripper_runtime != nullptr)
    result = RetainedComponentOwner(gripper_runtime).registrations();
  else
    Require((io_provider_drivers && !io_provider_drivers->empty()) || component_adapter,
            "native component has no execution registration owner");
  return result;
  }

Composition::Composition(
      core::ControllableComponentId selected_component_id,
      std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
      std::shared_ptr<const deployment::ProgramBindingContract>
          selected_program,
      std::shared_ptr<const deployment::ProgramBinding> selected_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_gripper_program_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_motion_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_gripper_binding,
      std::optional<core::JointLimitsDTO> selected_direct_control_joint_limits,
      CanonicalValue selected_direct_control_motion_descriptor,
      CanonicalValue selected_direct_control_gripper_descriptor,
      std::shared_ptr<const RetainedPlanningModelStage> selected_stage,
      std::shared_ptr<RetainedExecutionHost> selected_runtime,
      core::ProjectionRevisionDTO selected_projection,
      core::ActiveBundlePublicationGeneration selected_generation,
      std::optional<core::ProgramTargetReferenceDTO> selected_status_motion_target,
      std::optional<core::ProgramTargetReferenceDTO> selected_status_gripper_target)
      : ExecutionProduct(std::move(selected_bundle), std::move(selected_program),
                         std::move(selected_binding), std::move(selected_projection), selected_generation),
        controllable_component_id(std::move(selected_component_id)),
        direct_control_binding(std::move(selected_direct_control_binding)),
        direct_control_gripper_program_binding(
            std::move(selected_direct_control_gripper_program_binding)),
        direct_control_motion_binding(
            std::move(selected_direct_control_motion_binding)),
        direct_control_gripper_binding(
            std::move(selected_direct_control_gripper_binding)),
        direct_control_joint_limits(
            std::move(selected_direct_control_joint_limits)),
        direct_control_motion_descriptor(
            std::move(selected_direct_control_motion_descriptor)),
        direct_control_gripper_descriptor(
            std::move(selected_direct_control_gripper_descriptor)),
        stage(std::move(selected_stage)), runtime(std::move(selected_runtime)),
        status_motion_target(
            std::move(selected_status_motion_target)),
        status_gripper_target(
            std::move(selected_status_gripper_target)) {}

ExecutionProduct *ExecutionCompositionAccess::product() const {
  Require(!current || !logical_product, "cell product has conflicting physical/logical owners");
  return current ? static_cast<ExecutionProduct*>(current) : logical_product.get();
}

void BindHostIoRuleEvidenceProcessor(
    ExecutionProduct &product,
    const std::shared_ptr<ExecutionCompositionAccess> &actual_access) {
  if (!product.io_signal_runtime ||
      product.io_signal_runtime->has_rule_evidence_processor())
    return;
  Require(actual_access != nullptr, "I/O evidence has no actual cell access owner");
  product.io_signal_runtime->SetRuleEvidenceProcessor(
      [weak = std::weak_ptr(actual_access)]() -> core::Result<bool> {
        const auto access = weak.lock();
        if (!access)
          return core::Result<bool>::Ok(false);
        // Same existing pin protects every borrowed facade/component callback.
        // Neither this callback nor Core may upgrade/reenter it exclusively.
        std::shared_lock lock(access->mutex);
        auto *kernel = access->addressed_kernel.load(std::memory_order_acquire);
        if (!kernel)
          return core::Result<bool>::Ok(false);
        const auto processed = kernel->ProcessIoRuleEvidence();
        if (!processed.ok())
          return core::Result<bool>::Err(processed.error());
        return core::Result<bool>::Ok(true);
      },
      [weak = std::weak_ptr(actual_access)](
          const std::shared_ptr<core::IIoSignalService> &service,
          std::span<const core::InterruptSourceId> sources,
          bool disabled) -> core::Result<void> {
        const auto access = weak.lock();
        if (!access)
          return core::Result<void>::Err({"invalid_state_transition",
                                          "Input lifecycle owner expired",
                                          {}});
        std::shared_lock lock(access->mutex);
        auto *kernel = access->addressed_kernel.load(std::memory_order_acquire);
        if (!kernel)
          return core::Result<void>::Err(
              {"invalid_state_transition",
               "Input lifecycle Kernel is unavailable",
               {}});
        return kernel->SetGlobalIoSourcesPlannedDisabled(service, sources, disabled);
      });
}

void ExecutionCompositionAccess::detach_addressed_owner() {
  // Existing callbacks take this same shared lock through their Kernel call.
  // Acquiring it exclusively drains those calls before clearing the pointers.
  std::unique_lock lock(mutex);
  addressed_kernel.store(nullptr, std::memory_order_release);
  operation_journal.store(nullptr, std::memory_order_release);
  if (auto *selected = product(); selected && selected->io_signal_runtime)
    selected->io_signal_runtime->DetachRuleEvidenceProcessor();
}

std::vector<core::CoordinatedComponent> CoordinatedComponentsOf(const Composition *cell) {
  std::vector<core::CoordinatedComponent> components;
  if (cell == nullptr) return components;
  // An owner comes before its arm member (its composition
  // builds it), and every member carries its owner and role. The owner of a
  // machine with its arm deactivated is a view of its own (its adapter is
  // the view's).
  const auto add = [&](const Composition &view) {
    if (view.owner_component) {
      components.push_back(*view.owner_component);
      components.back().owner = true;
    }
    if (view.component_adapter &&
        !(view.owner_component &&
          view.owner_component->component_id == view.controllable_component_id.value()))
      components.push_back({view.controllable_component_id.value(), view.component_adapter,
                            view.owner_id, view.member_role});
    if (view.io_provider_drivers)
      for (const auto &driver : *view.io_provider_drivers)
        if (driver)
          components.push_back(
              {driver->domain().provider()->provider.controllable_component_id.value(), driver,
               view.owner_id, view.member_role});
  };
  add(*cell);
  for (const auto &participant : cell->participant_compositions)
    if (participant) add(*participant);
  return components;
}

std::vector<std::jthread> ExecutionCompositionAccess::replace_current(
    Composition *replacement) {
    HostRobotTcpReaders readers;
    const auto stage = [&](const Composition &composition) {
      if (composition.staged_robot_tcp_reader)
        readers.emplace(composition.controllable_component_id,
                        composition.staged_robot_tcp_reader);
    };
    if (replacement != nullptr) {
      stage(*replacement);
      for (const auto &participant : replacement->participant_compositions)
        stage(*participant);
    }
    // Caller holds mutex exclusively and keeps the predecessor alive. A new
    // Program/Scene publication may still use the same physical controllers;
    // only a controller actually retired by the component delta loses its
    // observed-state source. Waking a retained channel would falsely report
    // a controller disconnect to readers waiting on that stable source.
    if (current != nullptr && current != replacement) {
      std::set<const RetainedExecutionHost *> retained_motion;
      std::set<const RetainedGripperRuntime *> retained_grippers;
      const auto include_retained = [&](const Composition &composition) {
        if (composition.runtime != nullptr)
          retained_motion.insert(composition.runtime.get());
        if (composition.gripper_runtime != nullptr)
          retained_grippers.insert(composition.gripper_runtime.get());
      };
      if (replacement != nullptr) {
        include_retained(*replacement);
        for (const auto &participant : replacement->participant_compositions)
          include_retained(*participant);
      }
      const auto wake_if_retired = [&](const Composition &composition) {
        if (composition.runtime != nullptr &&
            !retained_motion.contains(composition.runtime.get())) {
          if (composition.runtime->motion_state_channel != nullptr)
            composition.runtime->motion_state_channel
                ->notify_composition_replaced();
        }
        if (composition.gripper_runtime != nullptr &&
            !retained_grippers.contains(composition.gripper_runtime.get())) {
          if (const auto channel = composition.gripper_runtime->state_channel();
              channel != nullptr)
            channel->notify_composition_replaced();
        }
      };
      wake_if_retired(*current);
      for (const auto &participant : current->participant_compositions)
        wake_if_retired(*participant);
    }
    SelectHostRobotTcps(std::move(readers), [&] { current = replacement; });
    logical_product.reset();
    if (publish_transport_membership) publish_transport_membership(replacement);
    // The selected cell's components are the ones the
    // component runtime coordinates; removed components no longer count.
    if (!component_runtime) return {};
    return component_runtime->SetComponents(CoordinatedComponentsOf(replacement));
  }

std::vector<std::jthread> ExecutionCompositionAccess::replace_logical_product(
    std::shared_ptr<ExecutionProduct> replacement) {
  // Caller holds mutex exclusively and retains any physical predecessor until
  // its existing retirement checks complete. Reuse those exact notifications.
  Require(!replacement || (replacement->bundle &&
              replacement->bundle->value().cell_graph.controllable_components.empty()),
          "logical cell product contains an actual physical component");
  auto removed = replace_current(nullptr);
  logical_product = std::move(replacement);
  return removed;
}
} // namespace manyforge::roboplan_backend::host_detail
