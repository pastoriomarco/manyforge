// Copyright 2026 Flexin Group SRL
// Licensed under the Apache License, Version 2.0.

#include <manyforge_roboplan/xarm_digital_input_provider.hpp>

#include <algorithm>
#include <limits>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <mutex>
#include <rclcpp/client.hpp>
#include <rclcpp/qos.hpp>
#include <set>
#include <stdexcept>
#include <utility>
#include <xarm_msgs/srv/get_digital_io.hpp>

namespace manyforge::roboplan_backend {
namespace {
using Service = xarm_msgs::srv::GetDigitalIO;
using Client = rclcpp::Client<Service>;
constexpr core::Duration kPollPeriodNs = 50'000'000; // 20 Hz

core::ErrorDTO invalid(const char *reason) {
  return {"io_source_unavailable", "xArm digital input is unavailable",
          {{"reason", reason}}};
}
} // namespace

struct XArmDigitalInputProvider::State final
    : std::enable_shared_from_this<State> {
  struct Pending final {
    std::int64_t request_id;
    std::uint64_t serial;
    core::IoSourceIngressAcquisitionReceipt receipt;
  };

  State(rclcpp::Node::SharedPtr selected_node,
        std::shared_ptr<core::IIoSignalIngress> ingress,
        std::shared_ptr<core::IClock> time,
        std::shared_ptr<core::IScheduler> timer, XArmDigitalInputBinding selected,
        core::Duration budget, std::uint64_t selected_generation)
      : node(std::move(selected_node)), clock(std::move(time)), scheduler(std::move(timer)),
        binding(std::move(selected)), timeout_ns(budget),
        sender(std::move(ingress), *clock, binding.source_id,
               binding.incarnation, selected_generation),
        generation(selected_generation) {}

  void ScheduleLocked() {
    const auto now = clock->now();
    if (now > std::numeric_limits<core::TimePoint>::max() - kPollPeriodNs)
      throw std::overflow_error("xArm input poll clock overflow");
    const auto expected = generation;
    scheduled = scheduler->schedule(
        now + kPollPeriodNs,
        [weak = weak_from_this(), expected] {
          if (const auto self = weak.lock())
            self->Poll(expected);
        },
        core::SchedulerPriority::Monitoring);
  }

  void Poll(std::uint64_t expected) {
    std::lock_guard lock(mutex);
    if (retired || !enabled || generation != expected)
      return;
    scheduled.reset();
    const auto now = clock->now();
    if (pending && (now < pending->receipt.received_at_ns ||
                    now - pending->receipt.received_at_ns >= timeout_ns)) {
      client->remove_pending_request(pending->request_id);
      pending.reset();
    }
    bool ready = false;
    try {
      ready = client->service_is_ready();
    } catch (const std::exception &) {
      // A stopped ROS context remains an unavailable input, not a poll crash.
    }
    if (!pending && ready &&
        serial != std::numeric_limits<std::uint64_t>::max()) {
      const auto receipt = sender.CaptureAcquisition(binding.incarnation, expected);
      const auto request_serial = ++serial;
      try {
        auto sent = client->async_send_request(
            std::make_shared<Service::Request>(),
            [weak = weak_from_this(), request_serial](Client::SharedFuture future) {
              if (const auto self = weak.lock())
                self->Receive(request_serial, std::move(future));
            });
        pending = Pending{sent.request_id, request_serial, receipt};
      } catch (const std::exception &) {
        // Reads have no actuator effect; the next scheduled poll is a new
        // observation, while the existing sample continues to age.
      }
    }
    try {
      ScheduleLocked();
    } catch (const std::exception &) {
      sender.NotifyLoss(core::IoIngressLossReason::kDisconnected,
                        sender.CaptureAcquisition(binding.incarnation, expected));
    }
  }

  void Receive(std::uint64_t request_serial, Client::SharedFuture future) {
    std::vector<core::IoSignalUpdateDTO> updates;
    std::optional<core::IoSourceIngressAcquisitionReceipt> receipt;
    {
      std::lock_guard lock(mutex);
      if (retired || !enabled || !pending || pending->serial != request_serial)
        return;
      receipt = pending->receipt;
      pending.reset();
      const auto now = clock->now();
      if (now < receipt->received_at_ns ||
          now - receipt->received_at_ns >= timeout_ns)
        return;
      Service::Response::SharedPtr response;
      try {
        response = future.get();
      } catch (const std::exception &) {
      }
      updates.reserve(binding.signals.size());
      for (const auto &signal : binding.signals) {
        const bool available = response && response->ret == 0;
        const bool valid = available && signal.pin < response->digitals.size() &&
                           (response->digitals[signal.pin] == 0 ||
                            response->digitals[signal.pin] == 1);
        updates.push_back(
            {signal.descriptor.signal_id,
             valid ? core::IoSampleValidity::kValid
                   : available ? core::IoSampleValidity::kInvalid
                               : core::IoSampleValidity::kUnavailable,
             valid ? std::optional<bool>(response->digitals[signal.pin] != 0)
                   : std::nullopt,
             std::nullopt});
      }
    }
    sender.Receive(std::move(updates), *receipt);
  }

  rclcpp::Node::SharedPtr node;
  std::shared_ptr<core::IClock> clock;
  std::shared_ptr<core::IScheduler> scheduler;
  XArmDigitalInputBinding binding;
  core::Duration timeout_ns;
  core::IoSourceIngressSender sender;
  rclcpp::CallbackGroup::SharedPtr callback_group;
  Client::SharedPtr client;
  std::mutex mutex;
  std::optional<core::ScheduleHandle> scheduled;
  std::optional<Pending> pending;
  std::uint64_t generation{};
  std::uint64_t serial{};
  bool enabled{};
  bool retired{};
};

core::Result<std::unique_ptr<XArmDigitalInputProvider>>
XArmDigitalInputProvider::Create(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<core::IIoSignalIngress> ingress,
    std::shared_ptr<core::IClock> clock,
    std::shared_ptr<core::IScheduler> scheduler, XArmDigitalInputBinding binding,
    std::optional<std::uint64_t> prepared_driver_generation) {
  using Reply = core::Result<std::unique_ptr<XArmDigitalInputProvider>>;
  if (!node || !ingress || !clock || !scheduler || binding.source_id.empty() ||
      binding.incarnation.empty() || binding.service.empty() ||
      binding.service.front() != '/' || binding.signals.empty() ||
      (binding.bank != XArmDigitalBank::kTool &&
       binding.bank != XArmDigitalBank::kController) ||
      !prepared_driver_generation || *prepared_driver_generation == 0U)
    return Reply::Err(invalid("configuration_invalid"));
  std::set<core::IoSignalId> identities;
  core::Duration timeout = 0;
  for (const auto &signal : binding.signals) {
    const auto &d = signal.descriptor;
    if (!identities.insert(d.signal_id).second || d.source_id != binding.source_id ||
        d.direction != core::IoSignalDirection::kInput ||
        d.value_type != core::IoSignalValueType::kBoolean ||
        d.update_mode != core::IoSignalUpdateMode::kSampled ||
        d.transition_capability != core::IoTransitionCapability::kSampledOnly ||
        d.liveness != core::IoSignalLivenessMode::kPeriodic ||
        d.freshness.mode != core::IoFreshnessMode::kSampleAge ||
        !d.freshness.maximum_age_ns || *d.freshness.maximum_age_ns <= 0 ||
        !core::ComputeIoSignalDescriptorDigest(d).ok() ||
        (binding.bank == XArmDigitalBank::kTool ? signal.pin > 1U
                                               : signal.pin > 15U))
      return Reply::Err(invalid("signal_contract_invalid"));
    timeout = std::max(timeout, *d.freshness.maximum_age_ns);
  }
  try {
    auto state = std::make_shared<State>(node, std::move(ingress), std::move(clock),
                                         std::move(scheduler), std::move(binding), timeout,
                                         *prepared_driver_generation);
    state->callback_group = node->create_callback_group(
        rclcpp::CallbackGroupType::MutuallyExclusive);
    state->client = node->create_client<Service>(state->binding.service,
                                                  rclcpp::ServicesQoS(),
                                                  state->callback_group);
    if (state->client->get_service_name() != state->binding.service)
      return Reply::Err(invalid("service_remapped"));
    return Reply::Ok(std::unique_ptr<XArmDigitalInputProvider>(
        new XArmDigitalInputProvider(std::move(state))));
  } catch (const std::exception &) {
    return Reply::Err(invalid("service_creation_failed"));
  }
}

XArmDigitalInputProvider::XArmDigitalInputProvider(std::shared_ptr<State> state)
    : state_(std::move(state)) {}
XArmDigitalInputProvider::~XArmDigitalInputProvider() { Shutdown(); }

bool XArmDigitalInputProvider::Pause(std::uint64_t generation) {
  std::lock_guard lock(state_->mutex);
  if (state_->retired || generation == 0U || generation < state_->generation)
    return false;
  if (generation == state_->generation)
    return !state_->enabled;
  if (!state_->sender.Pause(generation))
    return false;
  state_->generation = generation;
  state_->enabled = false;
  if (state_->scheduled)
    state_->scheduler->cancel(*std::exchange(state_->scheduled, std::nullopt));
  if (state_->pending)
    state_->client->remove_pending_request(state_->pending->request_id);
  state_->pending.reset();
  return true;
}

bool XArmDigitalInputProvider::Resume(std::uint64_t generation) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired || generation == 0U || generation < state_->generation)
      return false;
    if (generation == state_->generation)
      return state_->enabled;
    if (!state_->sender.Resume(generation))
      return false;
    state_->generation = generation;
    state_->enabled = true;
  }
  state_->Poll(generation);
  return true;
}

core::Result<void> XArmDigitalInputProvider::RebindIngress(
    std::shared_ptr<core::IIoSignalIngress> ingress,
    core::IoSourceIncarnation incarnation, std::uint64_t generation) {
  using Reply = core::Result<void>;
  bool enabled;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired || !ingress || incarnation.empty() ||
        generation <= state_->generation ||
        !state_->sender.RebindIngress(std::move(ingress), incarnation, generation))
      return Reply::Err(invalid("rebind_stale_or_invalid"));
    if (state_->scheduled)
      state_->scheduler->cancel(*std::exchange(state_->scheduled, std::nullopt));
    if (state_->pending)
      state_->client->remove_pending_request(state_->pending->request_id);
    state_->pending.reset();
    state_->binding.incarnation = std::move(incarnation);
    state_->generation = generation;
    enabled = state_->enabled;
  }
  if (enabled)
    state_->Poll(generation);
  return Reply::Ok();
}

void XArmDigitalInputProvider::Shutdown() {
  std::lock_guard lock(state_->mutex);
  if (state_->retired)
    return;
  state_->retired = true;
  state_->enabled = false;
  if (state_->scheduled)
    state_->scheduler->cancel(*std::exchange(state_->scheduled, std::nullopt));
  if (state_->pending)
    state_->client->remove_pending_request(state_->pending->request_id);
  state_->pending.reset();
  state_->sender.Shutdown();
}

} // namespace manyforge::roboplan_backend
