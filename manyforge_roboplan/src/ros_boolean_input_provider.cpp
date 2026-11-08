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
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <rclcpp/subscription.hpp>
#include <std_msgs/msg/bool.hpp>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {
core::ErrorDTO invalid(std::string message) {
  return {"invalid_request", std::move(message), {}};
}
} // namespace

struct RosBooleanInputProvider::State final {
  State(std::shared_ptr<core::IIoSignalIngress> sink,
        std::shared_ptr<core::IClock> clock,
        RosBooleanInputBinding resolved,
        std::optional<std::uint64_t> generation)
      : binding(std::move(resolved)),
        sender(std::move(sink), std::move(clock), binding.source_id,
               binding.incarnation,
               generation) {}

  void Receive(bool value, core::IoSourceIngressAcquisitionReceipt receipt) {
    sender.Receive({{binding.signal_id, core::IoSampleValidity::kValid, value,
                     std::nullopt}},
                   receipt);
  }
  const RosBooleanInputBinding binding;
  core::IoSourceIngressSender sender;
  std::atomic<std::shared_ptr<std::atomic<std::size_t>>> matched;
};

struct RosBooleanInputProvider::Entities final {
  Entities(rclcpp::Node::SharedPtr selected_node,
           const rclcpp::QoS &selected_qos,
           core::IoSourceIncarnation selected_incarnation,
           std::uint64_t selected_generation,
           std::shared_ptr<ros2_control_execution::Ros2TransportResources>
               selected_owner)
      : node(std::move(selected_node)), qos(selected_qos),
        incarnation(std::move(selected_incarnation)),
        generation(selected_generation), owner(selected_owner),
        owned(bool(selected_owner)) {}

  struct SubscriptionEntities final {
    std::shared_ptr<std::atomic<std::size_t>> matched =
        std::make_shared<std::atomic<std::size_t>>(std::numeric_limits<std::size_t>::max());
    rclcpp::Node::SharedPtr node;
    // OD-66: this subscription's own group; on an owner with a time-critical
    // thread it is a time-critical group (interrupt inputs), retained with
    // the subscription so its removal precedes the final release.
    rclcpp::CallbackGroup::SharedPtr callback_group;
    bool time_critical = false;
    rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr subscription;
  };

  std::shared_ptr<SubscriptionEntities>
  Subscribe(const std::shared_ptr<State> &state,
            core::IoSourceIncarnation callback_incarnation,
            std::uint64_t callback_generation,
            std::shared_ptr<std::atomic<bool>> active = {}) {
    auto result = std::make_shared<SubscriptionEntities>();
    result->node = node;
    result->callback_group = NewCallbackGroup(result->time_critical);
    rclcpp::SubscriptionOptions options;
    options.callback_group = result->callback_group;
    options.event_callbacks.matched_callback = [count = result->matched](rclcpp::MatchedInfo &info) {
      count->store(info.current_count, std::memory_order_release);
    };
    options.event_callbacks.message_lost_callback =
        [state, callback_incarnation,
         callback_generation, active](rclcpp::QOSMessageLostInfo &event) {
          if (active && !active->load(std::memory_order_acquire)) return;
          const auto receipt = state->sender.CaptureAcquisition(
              callback_incarnation, callback_generation);
          if (event.total_count_change > 0)
            state->sender.NotifyLoss(
                core::IoIngressLossReason::kMiddlewareMessageLost, receipt,
                static_cast<std::uint64_t>(event.total_count_change));
        };
    result->subscription = node->create_subscription<std_msgs::msg::Bool>(
        state->binding.topic,
        detail::InputConnectQos<std_msgs::msg::Bool>(*node,
                                                     state->binding.topic, qos),
        [state, callback_incarnation,
         callback_generation, active](const std_msgs::msg::Bool &message) {
          if (active && !active->load(std::memory_order_acquire)) return;
          const auto receipt = state->sender.CaptureAcquisition(
              callback_incarnation, callback_generation);
          state->Receive(message.data, receipt);
        },
        options);
    result->matched->store(result->subscription->get_publisher_count(), std::memory_order_release);
    return result;
  }
  template <class F> void Invoke(F &&work) {
    if (!owned) {
      work();
      return;
    }
    auto selected = owner.lock();
    if (!selected)
      throw std::runtime_error("ROS transport owner is unavailable");
    selected->invoke_creation(std::forward<F>(work));
  }
  rclcpp::CallbackGroup::SharedPtr NewCallbackGroup(bool &time_critical) {
    if (owned)
      if (const auto selected = owner.lock())
        if (auto group = selected->create_time_critical_callback_group()) {
          time_critical = true;
          return group;
        }
    time_critical = false;
    return node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  }
  std::shared_ptr<SubscriptionEntities>
  Retain(std::shared_ptr<SubscriptionEntities> value) {
    if (!owned)
      return value;
    auto selected = owner.lock();
    if (!selected)
      throw std::runtime_error("ROS transport owner is unavailable");
    auto group = value->time_critical ? value->callback_group : nullptr;
    return selected->retain_entity(std::move(value), std::move(group));
  }
  rclcpp::Node::SharedPtr node;
  rclcpp::QoS qos;
  core::IoSourceIncarnation incarnation;
  std::uint64_t generation;
  std::weak_ptr<ros2_control_execution::Ros2TransportResources> owner;
  bool owned;
  std::shared_ptr<SubscriptionEntities> subscription;
};

struct RosBooleanInputProvider::TransportRecoverySnapshot final {
  std::shared_ptr<State> state;
  std::shared_ptr<Entities> entities;
  rclcpp::QoS qos;
  core::IoSourceIncarnation incarnation;
  std::uint64_t generation;
};
struct RosBooleanInputProvider::TransportRecoveryStage final {
  std::shared_ptr<const TransportRecoverySnapshot> snapshot;
  std::shared_ptr<Entities> entities;
  std::shared_ptr<std::atomic<bool>> active;
};

std::shared_ptr<const RosBooleanInputProvider::TransportRecoverySnapshot>
RosBooleanInputProvider::CaptureTransportRecovery() const {
  return std::make_shared<TransportRecoverySnapshot>(
      TransportRecoverySnapshot{state_, entities_, entities_->qos,
                                entities_->incarnation, entities_->generation});
}
bool RosBooleanInputProvider::MatchesTransportRecovery(
    const TransportRecoverySnapshot &snapshot) const noexcept {
  return state_ == snapshot.state && entities_ == snapshot.entities &&
         entities_->incarnation == snapshot.incarnation &&
         entities_->generation == snapshot.generation;
}
core::Result<std::shared_ptr<RosBooleanInputProvider::TransportRecoveryStage>>
RosBooleanInputProvider::StageTransportRecovery(
    std::shared_ptr<const TransportRecoverySnapshot> snapshot,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner) {
  using Reply = core::Result<std::shared_ptr<TransportRecoveryStage>>;
  if (!snapshot || !owner || !owner->node())
    return Reply::Err(
        invalid("Input transport recovery requires a snapshot and owner"));
  try {
    auto stage = std::make_shared<TransportRecoveryStage>();
    stage->snapshot = std::move(snapshot);
    stage->active = std::make_shared<std::atomic<bool>>(false);
    stage->entities = owner->retain_entity(std::make_shared<Entities>(
        owner->node(), stage->snapshot->qos, stage->snapshot->incarnation,
        stage->snapshot->generation, owner));
    stage->entities->Invoke([&] {
      stage->entities->subscription =
          stage->entities->Retain(stage->entities->Subscribe(
              stage->snapshot->state, stage->entities->incarnation,
              stage->entities->generation, stage->active));
    });
    return Reply::Ok(std::move(stage));
  } catch (const core::TypedError &error) {
    return Reply::Err(error.error());
  } catch (const std::exception &error) {
    return Reply::Err({"io_ros_subscription_failed", error.what(), {}});
  }
}
void RosBooleanInputProvider::CommitTransportRecovery(
    TransportRecoveryStage &stage) noexcept {
  entities_ = std::move(stage.entities);
  state_->matched.store(entities_->subscription->matched, std::memory_order_release);
}
void RosBooleanInputProvider::ActivateTransportRecovery(
    TransportRecoveryStage &stage) noexcept {
  stage.active->store(true, std::memory_order_release);
}

core::Result<std::unique_ptr<RosBooleanInputProvider>>
RosBooleanInputProvider::Create(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<core::IIoSignalIngress> ingress,
    std::shared_ptr<core::IClock> clock,
    RosBooleanInputBinding binding,
    const core::IoSignalDescriptorDTO &descriptor, const rclcpp::QoS &qos,
    std::optional<std::uint64_t> prepared_driver_generation,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner) {
  using Reply = core::Result<std::unique_ptr<RosBooleanInputProvider>>;
  if (!node || !ingress || !clock || binding.topic.empty() ||
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
  auto entities = std::make_shared<Entities>(
      std::move(node), qos, state->binding.incarnation,
      prepared_driver_generation.value_or(0U), owner);
  if (owner)
    entities = owner->retain_entity(std::move(entities));
  try {
    entities->Invoke([&] {
      entities->subscription = entities->Retain(entities->Subscribe(
          state, entities->incarnation, entities->generation));
    });
  } catch (const core::TypedError &error) {
    if (error.code() == "ros_transport_executor_failed" ||
        (error.code() == "component_not_ready" &&
         error.details().contains("reason") &&
         error.details().at("reason") == "owner_closing"))
      return Reply::Err(error.error());
    return Reply::Err({"io_ros_subscription_failed", error.what(), {}});
  } catch (const std::exception &error) {
    return Reply::Err({"io_ros_subscription_failed", error.what(), {}});
  }
  state->matched.store(entities->subscription->matched, std::memory_order_release);
  return Reply::Ok(std::unique_ptr<RosBooleanInputProvider>(
      new RosBooleanInputProvider(std::move(state), std::move(entities))));
}

RosBooleanInputProvider::RosBooleanInputProvider(
    std::shared_ptr<State> state, std::shared_ptr<Entities> entities)
    : state_(std::move(state)), entities_(std::move(entities)) {}

RosBooleanInputProvider::~RosBooleanInputProvider() { Shutdown(); }

std::optional<std::size_t> RosBooleanInputProvider::matched_publishers() const noexcept {
  const auto matched = state_->matched.load(std::memory_order_acquire);
  if (!matched) return std::nullopt;
  const auto count = matched->load(std::memory_order_acquire);
  if (count == std::numeric_limits<std::size_t>::max()) return std::nullopt;
  return count;
}

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
    decltype(entities_->subscription) subscription;
    entities_->Invoke([&] {
      subscription = entities_->Retain(
          entities_->Subscribe(state_, incarnation, generation));
    });
    if (!state_->sender.RebindIngress(std::move(ingress), incarnation,
                                      generation))
      return core::Result<void>::Err(
          {"io_source_unavailable", "Input delivery rebind is stale", {}});
    entities_->incarnation = std::move(incarnation);
    entities_->generation = generation;
    entities_->subscription = std::move(subscription);
    state_->matched.store(entities_->subscription->matched, std::memory_order_release);
    return core::Result<void>::Ok();
  } catch (const core::TypedError &error) {
    if (error.code() == "ros_transport_executor_failed" ||
        (error.code() == "component_not_ready" &&
         error.details().contains("reason") &&
         error.details().at("reason") == "owner_closing"))
      return core::Result<void>::Err(error.error());
    return core::Result<void>::Err(
        {"io_ros_subscription_failed", error.what(), {}});
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
    decltype(entities_->subscription) subscription;
    entities_->Invoke([&] {
      subscription = entities_->Retain(
          entities_->Subscribe(state_, entities_->incarnation, generation));
    });
    if (!state_->sender.Resume(generation))
      return false;
    entities_->subscription = std::move(subscription);
    state_->matched.store(entities_->subscription->matched, std::memory_order_release);
    entities_->generation = generation;
    return true;
  } catch (const core::TypedError &error) {
    if (error.code() == "ros_transport_executor_failed" ||
        (error.code() == "component_not_ready" &&
         error.details().contains("reason") &&
         error.details().at("reason") == "owner_closing"))
      throw;
    return false;
  } catch (const std::exception &) {
    return false;
  }
}

void RosBooleanInputProvider::SetArrivalRecorder(
    std::shared_ptr<core::StreamRecorder> arrivals) {
  state_->sender.SetArrivalRecorder(std::move(arrivals));
}

void RosBooleanInputProvider::Shutdown() { state_->sender.Shutdown(); }

} // namespace manyforge::roboplan_backend
