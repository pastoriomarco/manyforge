// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <manyforge_roboplan/ros_boolean_input_provider.hpp>

#include "ros_input_connect_qos.hpp"
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/io_source_ingress_sender.hpp>
#include <rclcpp/subscription.hpp>
#include <std_msgs/msg/bool.hpp>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {
core::ErrorDTO invalid(std::string message) {
  return {"invalid_request", std::move(message), {}};
}
} // namespace

struct RosBooleanInputProvider::State final {
  State(std::shared_ptr<core::IIoSignalIngress> sink, core::IClock &time,
        RosBooleanInputBinding resolved,
        std::optional<std::uint64_t> generation)
      : binding(std::move(resolved)),
        sender(std::move(sink), time, binding.source_id, binding.incarnation,
               generation) {}

  void Receive(bool value, core::IoSourceIngressAcquisitionReceipt receipt) {
    sender.Receive({{binding.signal_id, core::IoSampleValidity::kValid, value,
                     std::nullopt}},
                   receipt);
  }
  const RosBooleanInputBinding binding;
  core::IoSourceIngressSender sender;
};

struct RosBooleanInputProvider::Entities final {
  Entities(rclcpp::Node::SharedPtr selected_node,
           const rclcpp::QoS &selected_qos,
           core::IoSourceIncarnation selected_incarnation,
           std::uint64_t selected_generation)
      : node(std::move(selected_node)), qos(selected_qos),
        incarnation(std::move(selected_incarnation)),
        generation(selected_generation) {}

  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr
  Subscribe(const std::shared_ptr<State> &state,
            core::IoSourceIncarnation callback_incarnation,
            std::uint64_t callback_generation) {
    rclcpp::SubscriptionOptions options;
    options.callback_group = callback_group;
    options.event_callbacks.message_lost_callback =
        [state, callback_incarnation,
         callback_generation](rclcpp::QOSMessageLostInfo &event) {
          const auto receipt = state->sender.CaptureAcquisition(
              callback_incarnation, callback_generation);
          if (event.total_count_change > 0)
            state->sender.NotifyLoss(
                core::IoIngressLossReason::kMiddlewareMessageLost, receipt,
                static_cast<std::uint64_t>(event.total_count_change));
        };
    return node->create_subscription<std_msgs::msg::Bool>(
        state->binding.topic,
        detail::InputConnectQos<std_msgs::msg::Bool>(*node,
                                                     state->binding.topic, qos),
        [state, callback_incarnation,
         callback_generation](const std_msgs::msg::Bool &message) {
          const auto receipt = state->sender.CaptureAcquisition(
              callback_incarnation, callback_generation);
          state->Receive(message.data, receipt);
        },
        options);
  }
  rclcpp::Node::SharedPtr node;
  rclcpp::QoS qos;
  core::IoSourceIncarnation incarnation;
  std::uint64_t generation;
  rclcpp::CallbackGroup::SharedPtr callback_group;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr subscription;
};

core::Result<std::unique_ptr<RosBooleanInputProvider>>
RosBooleanInputProvider::Create(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
    RosBooleanInputBinding binding,
    const core::IoSignalDescriptorDTO &descriptor, const rclcpp::QoS &qos,
    std::optional<std::uint64_t> prepared_driver_generation) {
  using Reply = core::Result<std::unique_ptr<RosBooleanInputProvider>>;
  if (!node || !ingress || binding.topic.empty() ||
      binding.incarnation.value().empty() ||
      binding.source_id.value().empty() ||
      binding.signal_id.controllable_component_id.value().empty() ||
      binding.signal_id.name.empty() ||
      (prepared_driver_generation && *prepared_driver_generation == 0U))
    return Reply::Err(invalid(
        "ROS boolean input requires node, ingress, topic and incarnation"));
  const auto &profile = qos.get_rmw_qos_profile();
  if (profile.durability != RMW_QOS_POLICY_DURABILITY_VOLATILE ||
      profile.history != RMW_QOS_POLICY_HISTORY_KEEP_LAST || profile.depth == 0)
    return Reply::Err(
        invalid("ROS boolean input requires explicit bounded volatile QoS"));
  if (descriptor.source_id != binding.source_id ||
      descriptor.signal_id != binding.signal_id)
    return Reply::Err(invalid(
        "ROS boolean input descriptor does not match resolved binding"));
  if (!core::ComputeIoSignalDescriptorDigest(descriptor).ok() ||
      descriptor.direction != core::IoSignalDirection::kInput ||
      descriptor.value_type != core::IoSignalValueType::kBoolean ||
      descriptor.liveness == core::IoSignalLivenessMode::kInProcess ||
      descriptor.transition_capability !=
          core::IoTransitionCapability::kSampledOnly)
    return Reply::Err(
        {"unsupported_capability",
         "ROS Bool requires an explicit periodic or change-only input contract",
         {}});

  auto state =
      std::make_shared<State>(std::move(ingress), clock, std::move(binding),
                              prepared_driver_generation);
  auto entities = std::make_unique<Entities>(
      std::move(node), qos, state->binding.incarnation,
      prepared_driver_generation.value_or(0U));
  try {
    entities->callback_group = entities->node->create_callback_group(
        rclcpp::CallbackGroupType::MutuallyExclusive);
    entities->subscription =
        entities->Subscribe(state, entities->incarnation, entities->generation);
  } catch (const std::exception &error) {
    return Reply::Err({"io_ros_subscription_failed", error.what(), {}});
  }
  return Reply::Ok(std::unique_ptr<RosBooleanInputProvider>(
      new RosBooleanInputProvider(std::move(state), std::move(entities))));
}

RosBooleanInputProvider::RosBooleanInputProvider(
    std::shared_ptr<State> state, std::unique_ptr<Entities> entities)
    : state_(std::move(state)), entities_(std::move(entities)) {}

RosBooleanInputProvider::~RosBooleanInputProvider() { Shutdown(); }

RosBooleanInputStatus RosBooleanInputProvider::status() const {
  const auto current = state_->sender.status();
  return {current.accepting, current.last_ingress_source_sequence,
          current.last_enqueued_source_sequence, current.last_loss};
}

bool RosBooleanInputProvider::MarkUnavailable(
    const core::IoSourceIncarnation &expected_incarnation,
    std::uint64_t expected_ingress_source_sequence) {
  return state_->sender.MarkUnavailable(expected_incarnation,
                                        expected_ingress_source_sequence);
}

core::Result<void> RosBooleanInputProvider::RebindIngress(
    std::shared_ptr<core::IIoSignalIngress> ingress,
    core::IoSourceIncarnation incarnation, std::uint64_t generation) {
  if (!ingress || incarnation.empty() || generation == 0U)
    return core::Result<void>::Err(
        {"invalid_request", "Input delivery rebind is invalid", {}});
  try {
    if (incarnation == entities_->incarnation) {
      if (!state_->sender.RebindIngress(std::move(ingress), incarnation,
                                        generation))
        return core::Result<void>::Err(
            {"io_source_unavailable", "Input delivery rebind is stale", {}});
      return core::Result<void>::Ok();
    }
    // A new subscription captures the actual host-issued source incarnation.
    // Old executor/DDS callbacks keep their old incarnation after handoff.
    auto subscription = entities_->Subscribe(state_, incarnation, generation);
    if (!state_->sender.RebindIngress(std::move(ingress), incarnation,
                                      generation))
      return core::Result<void>::Err(
          {"io_source_unavailable", "Input delivery rebind is stale", {}});
    entities_->incarnation = std::move(incarnation);
    entities_->generation = generation;
    entities_->subscription = std::move(subscription);
    return core::Result<void>::Ok();
  } catch (const std::exception &error) {
    return core::Result<void>::Err(
        {"io_ros_subscription_failed", error.what(), {}});
  }
}

bool RosBooleanInputProvider::Pause(std::uint64_t generation) {
  return state_->sender.Pause(generation);
}
bool RosBooleanInputProvider::Resume(std::uint64_t generation) {
  try {
    if (generation == entities_->generation)
      return state_->sender.Resume(generation);
    // Old queued executor/DDS callbacks retain the old subscription token.
    // Volatile replacement starts a new callback channel before gate commit.
    auto subscription =
        entities_->Subscribe(state_, entities_->incarnation, generation);
    if (!state_->sender.Resume(generation))
      return false;
    entities_->subscription = std::move(subscription);
    entities_->generation = generation;
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

void RosBooleanInputProvider::Shutdown() { state_->sender.Shutdown(); }

} // namespace manyforge::roboplan_backend
