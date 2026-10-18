// Copyright 2026 Flexin Group SRL
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

#include <manyforge_roboplan/ros_mapped_boolean_input_provider.hpp>

#include "ros_input_connect_qos.hpp"
#include <cmath>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/io_source_ingress_sender.hpp>
#include <rclcpp/subscription.hpp>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <ur_msgs/msg/io_states.hpp>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {
core::ErrorDTO invalid(std::string message) {
  return {"invalid_request", std::move(message), {}};
}
} // namespace

struct RosMappedBooleanInputProvider::State final {
  State(std::shared_ptr<core::IIoSignalIngress> sink, core::IClock &time,
        RosMappedBooleanInputBinding resolved,
        std::optional<std::uint64_t> generation)
      : binding(std::move(resolved)),
        sender(std::move(sink), time, binding.source_id, binding.incarnation,
               generation) {}

  template <class Message>
  void Receive(const Message &message,
               core::IoSourceIngressAcquisitionReceipt receipt) {
    std::vector<core::IoSignalUpdateDTO> updates;
    updates.reserve(binding.signals.size());
    bool malformed = false;
    if constexpr (std::is_same_v<
                      Message,
                      control_msgs::msg::DynamicInterfaceGroupValues>) {
      malformed =
          message.interface_groups.size() != message.interface_values.size();
      for (const auto &values : message.interface_values)
        malformed =
            malformed || values.interface_names.size() != values.values.size();
    }
    for (const auto &configured : binding.signals) {
      std::optional<bool> value;
      std::size_t matches = 0;
      if constexpr (std::is_same_v<Message, ur_msgs::msg::IOStates>) {
        const auto pin =
            std::get<UrDigitalInputSelector>(configured.selector).pin;
        for (const auto &digital : message.digital_in_states)
          if (digital.pin == pin) {
            ++matches;
            value = digital.state;
          }
      } else {
        const auto &selected =
            std::get<RosGpioBooleanInputSelector>(configured.selector);
        if (!malformed)
          for (std::size_t i = 0; i < message.interface_groups.size(); ++i)
            if (message.interface_groups[i] == selected.group)
              for (std::size_t j = 0;
                   j < message.interface_values[i].interface_names.size(); ++j)
                if (message.interface_values[i].interface_names[j] ==
                    selected.interface_name) {
                  ++matches;
                  const auto raw = message.interface_values[i].values[j];
                  if (std::isfinite(raw) && (raw == 0.0 || raw == 1.0))
                    value = raw == 1.0;
                }
      }
      const bool valid = !malformed && matches == 1U && value.has_value();
      updates.push_back({configured.descriptor.signal_id,
                         valid ? core::IoSampleValidity::kValid
                               : core::IoSampleValidity::kInvalid,
                         valid ? value : std::nullopt, std::nullopt});
    }
    sender.Receive(std::move(updates), receipt);
  }
  const RosMappedBooleanInputBinding binding;
  core::IoSourceIngressSender sender;
};

struct RosMappedBooleanInputProvider::Entities final {
  Entities(rclcpp::Node::SharedPtr selected_node,
           const rclcpp::QoS &selected_qos,
           core::IoSourceIncarnation selected_incarnation,
           std::uint64_t selected_generation)
      : node(std::move(selected_node)), qos(selected_qos),
        incarnation(std::move(selected_incarnation)),
        generation(selected_generation) {}

  rclcpp::SubscriptionBase::SharedPtr
  Subscribe(const std::shared_ptr<State> &state,
            core::IoSourceIncarnation callback_incarnation,
            std::uint64_t callback_generation) {
    // Node remapping is immutable, but must not redirect this exact sealed
    // typed address. Check before creating any potentially live callback.
    if (node->get_node_topics_interface()->resolve_topic_name(
            state->binding.topic) != state->binding.topic)
      throw std::invalid_argument(
          "Mapped input topic differs from the sealed address");
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
    if (std::holds_alternative<UrDigitalInputSelector>(
            state->binding.signals.front().selector))
      return node->create_subscription<ur_msgs::msg::IOStates>(
          state->binding.topic,
          detail::InputConnectQos<ur_msgs::msg::IOStates>(
              *node, state->binding.topic, qos),
          [state, callback_incarnation,
           callback_generation](const ur_msgs::msg::IOStates &message) {
            const auto receipt = state->sender.CaptureAcquisition(
                callback_incarnation, callback_generation);
            state->Receive(message, receipt);
          },
          options);
    return node->create_subscription<
        control_msgs::msg::DynamicInterfaceGroupValues>(
        state->binding.topic,
        detail::InputConnectQos<control_msgs::msg::DynamicInterfaceGroupValues>(
            *node, state->binding.topic, qos),
        [state, callback_incarnation, callback_generation](
            const control_msgs::msg::DynamicInterfaceGroupValues &message) {
          const auto receipt = state->sender.CaptureAcquisition(
              callback_incarnation, callback_generation);
          state->Receive(message, receipt);
        },
        options);
  }
  rclcpp::Node::SharedPtr node;
  rclcpp::QoS qos;
  core::IoSourceIncarnation incarnation;
  std::uint64_t generation;
  rclcpp::CallbackGroup::SharedPtr callback_group;
  rclcpp::SubscriptionBase::SharedPtr subscription;
};

core::Result<std::unique_ptr<RosMappedBooleanInputProvider>>
RosMappedBooleanInputProvider::Create(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
    RosMappedBooleanInputBinding binding, const rclcpp::QoS &qos,
    std::optional<std::uint64_t> prepared_driver_generation) {
  using Reply = core::Result<std::unique_ptr<RosMappedBooleanInputProvider>>;
  if (!node || !ingress || binding.topic.empty() ||
      binding.topic.front() != '/' || binding.topic.size() > 4096U ||
      binding.topic.find('\0') != std::string::npos ||
      binding.incarnation.value().empty() ||
      binding.source_id.value().empty() || binding.signals.empty() ||
      (prepared_driver_generation && *prepared_driver_generation == 0U))
    return Reply::Err(invalid("Mapped ROS boolean input requires node, "
                              "ingress, topic and incarnation"));
  const auto &profile = qos.get_rmw_qos_profile();
  if (profile.durability != RMW_QOS_POLICY_DURABILITY_VOLATILE ||
      profile.history != RMW_QOS_POLICY_HISTORY_KEEP_LAST || profile.depth == 0)
    return Reply::Err(invalid(
        "Mapped ROS boolean input requires explicit bounded volatile QoS"));
  std::set<core::IoSignalId> identities;
  const auto selected_kind = binding.signals.front().selector.index();
  for (const auto &signal : binding.signals) {
    const auto &descriptor = signal.descriptor;
    if (!core::ComputeIoSignalDescriptorDigest(descriptor).ok() ||
        descriptor.source_id != binding.source_id ||
        descriptor.direction != core::IoSignalDirection::kInput ||
        descriptor.liveness != core::IoSignalLivenessMode::kPeriodic ||
        descriptor.update_mode != core::IoSignalUpdateMode::kSampled ||
        descriptor.transition_capability !=
            core::IoTransitionCapability::kSampledOnly ||
        !identities.insert(descriptor.signal_id).second ||
        signal.selector.index() != selected_kind)
      return Reply::Err(invalid("Mapped ROS input requires one periodic "
                                "sampled source and unique signals"));
    if (const auto *pin =
            std::get_if<UrDigitalInputSelector>(&signal.selector)) {
      if (pin->pin > 17U)
        return Reply::Err(invalid("UR digital input pin is outside 0..17"));
    } else {
      const auto &selected =
          std::get<RosGpioBooleanInputSelector>(signal.selector);
      const auto valid_text = [](const std::string &value) {
        return !value.empty() && value.size() <= 4096U &&
               value.find('\0') == std::string::npos;
      };
      if (!valid_text(selected.group) || !valid_text(selected.interface_name))
        return Reply::Err(
            invalid("GPIO selector requires exact group and interface names"));
    }
  }

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
  return Reply::Ok(std::unique_ptr<RosMappedBooleanInputProvider>(
      new RosMappedBooleanInputProvider(std::move(state),
                                        std::move(entities))));
}

RosMappedBooleanInputProvider::RosMappedBooleanInputProvider(
    std::shared_ptr<State> state, std::unique_ptr<Entities> entities)
    : state_(std::move(state)), entities_(std::move(entities)) {}

RosMappedBooleanInputProvider::~RosMappedBooleanInputProvider() { Shutdown(); }

core::IoSourceIngressSenderStatus
RosMappedBooleanInputProvider::status() const {
  return state_->sender.status();
}

bool RosMappedBooleanInputProvider::MarkUnavailable(
    const core::IoSourceIncarnation &expected_incarnation,
    std::uint64_t expected_ingress_source_sequence) {
  return state_->sender.MarkUnavailable(expected_incarnation,
                                        expected_ingress_source_sequence);
}

core::Result<void> RosMappedBooleanInputProvider::RebindIngress(
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

bool RosMappedBooleanInputProvider::Pause(std::uint64_t generation) {
  return state_->sender.Pause(generation);
}
bool RosMappedBooleanInputProvider::Resume(std::uint64_t generation) {
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

void RosMappedBooleanInputProvider::Shutdown() { state_->sender.Shutdown(); }

} // namespace manyforge::roboplan_backend
