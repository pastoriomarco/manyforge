#pragma once

#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {

/// One independently managed native component. A robot may own its embedded
/// gripper through the motion runtime; a separately controlled gripper has no
/// motion runtime or fabricated planner. The cell retains these values across
/// publications and replaces only the components named by an Apply delta.
class RetainedComponentOwner final {
 public:
  using Owner = std::variant<std::shared_ptr<RetainedExecutionHost>,
                             std::shared_ptr<RetainedGripperRuntime>>;

  explicit RetainedComponentOwner(
      std::shared_ptr<RetainedExecutionHost> motion)
      : owner_(std::move(motion)) {
    const auto *runtime = motion_runtime();
    if (runtime == nullptr || runtime->participant_engine == nullptr ||
        runtime->participant_registrations.empty()) {
      throw std::invalid_argument("native motion component has no execution owner");
    }
    const auto component_id =
        runtime->motion_binding.value().controllable_component_id;
    validate_registrations(component_id,
                           component_registrations(*runtime, component_id),
                           physical_bindings());
  }

  explicit RetainedComponentOwner(
      std::shared_ptr<RetainedGripperRuntime> gripper)
      : owner_(std::move(gripper)) {
    const auto *runtime = gripper_runtime();
    if (runtime == nullptr || runtime->participant_engine() == nullptr) {
      throw std::invalid_argument("native gripper component has no execution owner");
    }
    validate_registrations(runtime->binding().value().controllable_component_id,
                           runtime->registrations(), physical_bindings());
  }

  [[nodiscard]] core::ControllableComponentId component_id() const {
    if (const auto *runtime = motion_runtime()) {
      return runtime->motion_binding.value().controllable_component_id;
    }
    return gripper_runtime()->binding().value().controllable_component_id;
  }

  [[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
  registrations() const {
    if (const auto *runtime = motion_runtime()) {
      return component_registrations(
          *runtime, runtime->motion_binding.value().controllable_component_id);
    }
    return gripper_runtime()->registrations();
  }

  [[nodiscard]] std::vector<core::RuntimeParticipantBinding>
  physical_bindings() const {
    if (const auto *runtime = motion_runtime()) {
      return {runtime->motion_binding};
    }
    return {gripper_runtime()->binding()};
  }

  [[nodiscard]] std::vector<std::shared_ptr<IRetainedObservedStateChannel>>
  observed_channels() const {
    if (const auto *runtime = motion_runtime()) {
      std::vector<std::shared_ptr<IRetainedObservedStateChannel>> channels;
      if (runtime->motion_state_channel != nullptr) {
        channels.push_back(runtime->motion_state_channel);
      }
      return channels;
    }
    return {gripper_runtime()->state_channel()};
  }

  [[nodiscard]] RetainedExecutionHost *motion_runtime() const noexcept {
    const auto *runtime =
        std::get_if<std::shared_ptr<RetainedExecutionHost>>(&owner_);
    return runtime != nullptr ? runtime->get() : nullptr;
  }

  [[nodiscard]] RetainedGripperRuntime *gripper_runtime() const noexcept {
    const auto *runtime =
        std::get_if<std::shared_ptr<RetainedGripperRuntime>>(&owner_);
    return runtime != nullptr ? runtime->get() : nullptr;
  }

 private:
  static std::vector<core::RuntimeParticipantExecutionRegistration>
  component_registrations(
      const RetainedExecutionHost &runtime,
      const core::ControllableComponentId &component_id) {
    std::vector<core::RuntimeParticipantExecutionRegistration> result;
    for (const auto &registration : runtime.participant_registrations) {
      if (registration.binding.value().controllable_component_id ==
          component_id) {
        result.push_back(registration);
      }
    }
    return result;
  }

  static void validate_registrations(
      const core::ControllableComponentId &component_id,
      const std::vector<core::RuntimeParticipantExecutionRegistration>
          &registrations,
      const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
    if (registrations.empty() || physical_bindings.empty()) {
      throw std::invalid_argument(
          "native component has no physical execution participants");
    }
    for (const auto &registration : registrations) {
      if (registration.engine == nullptr ||
          registration.binding.value().controllable_component_id !=
              component_id) {
        throw std::invalid_argument(
            "native component registration crosses its execution owner");
      }
    }
    for (const auto &physical : physical_bindings) {
      if (physical.value().controllable_component_id != component_id ||
          std::count_if(registrations.begin(), registrations.end(),
                        [&](const auto &registration) {
                          return registration.binding.value() ==
                                 physical.value();
                        }) != 1) {
        throw std::invalid_argument(
            "native component has no unique registration for a physical target");
      }
    }
  }

  Owner owner_;
};

}  // namespace manyforge::roboplan_backend
