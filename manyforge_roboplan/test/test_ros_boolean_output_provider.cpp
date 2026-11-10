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

#include <manyforge_roboplan/ros_boolean_output_provider.hpp>
#ifdef MANYFORGE_LEAF_TIMING
#include <manyforge_ros2_control_execution/detail/leaf_timing.hpp>
#endif
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <functional>
#include <future>
#include <iostream>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <stop_token>
#include <thread>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {
using namespace std::chrono_literals;
using Service = std_srvs::srv::SetBool;

const core::ContentDigest kDigest("sha256:" + std::string(64U, 'a'));

core::ResolvedLifecycleProviderOccurrenceDTO occurrence(std::string name) {
  return {core::ControlDomainId(name + ".domain"),
          core::ControlDomainRevision(kDigest.value()),
          core::ProviderInstanceId(name + ".provider"),
          core::ProviderManifestId("ros_set_bool"),
          core::ProviderManifestRevision(kDigest.value()),
          core::RuntimeUnitId("io.host"),
          core::RuntimeEpoch("runtime.1"),
          kDigest};
}

core::IoSignalId signal(std::string component, std::string name = "output") {
  return {core::ControllableComponentId(std::move(component)), std::move(name)};
}

// Leaf unit fixtures use distinct shared control blocks; actual host callers
// supply the existing IoProviderDriverRecord control block instead.
std::shared_ptr<const void> owner(std::string_view name) {
  static const auto a = std::make_shared<const int>(1);
  static const auto b = std::make_shared<const int>(2);
  static const auto foreign = std::make_shared<const int>(3);
  if (name == "a")
    return a;
  if (name == "b")
    return b;
  return foreign;
}

template <class Alias>
std::vector<IoOutputAliasIdentity>
identities(const std::vector<Alias> &aliases) {
  std::vector<IoOutputAliasIdentity> result;
  for (const auto &alias : aliases)
    result.push_back(
        {alias.signal_id, alias.provider, alias.actual_driver_owner});
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.signal_id < b.signal_id;
  });
  return result;
}

RosBooleanOutputChannel channel() {
  return {core::ResourceId("mock.output.shared"),
          RosSetBoolOutputAddress{"/test/manyforge_set_bool"},
          {{signal("component.a"), occurrence("a"), 1U, owner("a")},
           {signal("component.a", "alias"), occurrence("a"), 1U, owner("a")},
           {signal("component.b"), occurrence("b"), 1U, owner("b")}},
          core::ActiveBundlePublicationGeneration(1U),
          500'000'000};
}

RosBooleanOutputChannel
gpio_channel(std::string label, std::string group, std::string interface_name,
             std::string topic = "/test/group_commands") {
  return {core::ResourceId("gpio." + label),
          RosGpioOutputAddress{std::move(topic), std::move(group),
                               std::move(interface_name)},
          {{signal("component." + label), occurrence(label), 1U, owner(label)}},
          core::ActiveBundlePublicationGeneration(1U)};
}
using GpioValues = std::map<std::string, std::map<std::string, double>>;
GpioValues
gpio_values(const control_msgs::msg::DynamicInterfaceGroupValues &message) {
  GpioValues result;
  for (std::size_t i = 0; i < message.interface_groups.size(); ++i)
    for (std::size_t j = 0; j < message.interface_values.at(i).values.size();
         ++j)
      result[message.interface_groups[i]]
            [message.interface_values[i].interface_names.at(j)] =
                message.interface_values[i].values[j];
  return result;
}

class RosBooleanOutputProviderTest : public ::testing::Test {
protected:
  void SetUp() override {
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions options;
    options.context(context);
    options.use_global_arguments(false);
    fixture_owner = ros2_control_execution::Ros2TransportResources::create(
        "manyforge_output_client_test", "", 1U, [this] { return context; });
    node = fixture_owner->node();
    fixture_owner->start();
    server_node =
        std::make_shared<rclcpp::Node>("manyforge_output_server_test", options);
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    server_executor =
        std::make_unique<rclcpp::executors::SingleThreadedExecutor>(
            executor_options);
    server_executor->add_node(server_node);
  }
  void TearDown() override {
    if (provider)
      provider->Shutdown();
    if (server_executor) {
      server_executor->cancel();
      if (server_node) server_executor->remove_node(server_node);
    }
    provider.reset();
    service.reset();
    server_executor.reset();
    if (fixture_owner) fixture_owner->quiesce();
    node.reset();
    fixture_owner.reset();
    server_node.reset();
    if (context && context->is_valid()) context->shutdown("boolean output fixture complete");
  }
  bool SpinUntil(const std::function<bool()> &condition,
                 bool spin_server = true) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      if (spin_server)
        server_executor->spin_some();
      std::this_thread::sleep_for(2ms);
    }
    return condition();
  }
  auto CreateFixtureOutput(rclcpp::Node::SharedPtr selected_node,
                           RosBooleanOutputChannel selected_channel,
                           std::shared_ptr<RosBooleanOutputProvider::GpioTopic> topic = {}) {
    return RosBooleanOutputProvider::Create(std::move(selected_node), std::move(selected_channel),
                                            std::move(topic), fixture_owner);
  }
  void Build(bool with_service = true) {
    if (with_service) {
      service = server_node->create_service<Service>(
          std::get<RosSetBoolOutputAddress>(channel().address).service,
          [this](Service::Request::SharedPtr request,
                 Service::Response::SharedPtr response) {
            received.push_back(request->data);
            response->success = accept;
            response->message = "diagnostic only, not hardware state";
          });
    }
    auto created = CreateFixtureOutput(node, channel());
    ASSERT_TRUE(created.ok()) << created.error().message;
    provider = std::move(created.value());
    if (with_service) {
      ASSERT_TRUE(SpinUntil([&] { return provider->ServiceReady(); }));
    }
  }
  // A service write blocks until answered; spin both executors meanwhile.
  SetOutputResult Write(RosBooleanOutputProvider &target, core::IoSignalId id,
                        bool value, bool spin_server = true,
                        std::stop_token stop = {}) {
    auto written = std::async(std::launch::async, [&] {
      return target.WriteOutput(id, value, stop);
    });
    EXPECT_TRUE(SpinUntil(
        [&] { return written.wait_for(0s) == std::future_status::ready; },
        spin_server));
    return written.get();
  }

  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr node, server_node;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> fixture_owner;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> server_executor;
  rclcpp::Service<Service>::SharedPtr service;
  std::unique_ptr<RosBooleanOutputProvider> provider;
  std::vector<bool> received;
  bool accept = true;
};

TEST_F(RosBooleanOutputProviderTest, OutputRequiresItsExactTransportOwner) {
  const auto absent = RosBooleanOutputProvider::Create(node, channel(), {}, {});
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().code, "ros_output_configuration_invalid");
  auto other = ros2_control_execution::Ros2TransportResources::create(
      "other_output_owner", "", 1U, [this] { return context; });
  const auto mismatch = RosBooleanOutputProvider::Create(node, channel(), {}, other);
  ASSERT_FALSE(mismatch.ok());
  EXPECT_EQ(mismatch.error().code, "ros_output_configuration_invalid");
  other->quiesce();
}

TEST_F(RosBooleanOutputProviderTest, OwnerThreadSetBoolRefusesBeforeSend) {
  auto transport =
      ros2_control_execution::Ros2TransportResources::create(
          "manyforge_output_owned_test", "", 1U, [this] { return context; });
  std::atomic<unsigned> calls{0};
  service = server_node->create_service<Service>(
      std::get<RosSetBoolOutputAddress>(channel().address).service,
      [&](Service::Request::SharedPtr, Service::Response::SharedPtr response) {
        ++calls;
        response->success = true;
      });
  auto created = RosBooleanOutputProvider::Create(transport->node(), channel(), {}, transport);
  ASSERT_TRUE(created.ok());
  auto owned_provider = std::move(created.value());
  ASSERT_TRUE(owned_provider->Resume(occurrence("a"), 2, owner("a")).ok());
  transport->start();
  ASSERT_TRUE(SpinUntil([&] { return owned_provider->ServiceReady(); }));
  SetOutputResult result{};
  transport->invoke([&] { result = owned_provider->WriteOutput(signal("component.a"), true, {}); });
  EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "io_output_not_submitted");
  server_executor->spin_some();
  EXPECT_EQ(calls.load(), 0U);
  owned_provider.reset();
  transport->quiesce();
}

TEST_F(RosBooleanOutputProviderTest, FaultedOwnerRefusesOutputCreationBeforeJoin) {
  auto transport = ros2_control_execution::Ros2TransportResources::create(
      "manyforge_output_faulted_test", "", 1U, [this] { return context; });
  std::promise<void> entered;
  auto observed = entered.get_future();
  std::promise<void> release;
  auto released = release.get_future().share();
  transport->set_executor_fault_observer([&](std::string_view) {
    entered.set_value();
    released.wait();
  });
  transport->start();
  rclcpp::TimerBase::SharedPtr timer;
  transport->invoke_creation([&] {
    timer = transport->node()->create_wall_timer(
        1ms, [] { throw std::runtime_error("forced owner fault"); });
  });
  const bool fault_entered = observed.wait_for(5s) == std::future_status::ready;
  std::optional<core::ErrorDTO> failure;
  if (fault_entered) {
    auto made = RosBooleanOutputProvider::Create(transport->node(), channel(), {}, transport);
    if (!made.ok()) failure = made.error();
  }
  release.set_value();
  transport->quiesce();
  timer.reset();
  ASSERT_TRUE(fault_entered);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "ros_transport_executor_failed");
}

TEST_F(RosBooleanOutputProviderTest, CachedServiceReadinessTracksGraphChanges) {
  Build(false);
  EXPECT_EQ(provider->CachedServiceReady(), false);
  service = server_node->create_service<Service>(
      std::get<RosSetBoolOutputAddress>(channel().address).service,
      [](Service::Request::SharedPtr, Service::Response::SharedPtr response) {
        response->success = true;
      });
  ASSERT_TRUE(SpinUntil([&] { return provider->CachedServiceReady() == true; }));
  service.reset();
  EXPECT_TRUE(SpinUntil([&] { return provider->CachedServiceReady() == false; }));
}

TEST_F(RosBooleanOutputProviderTest, DisabledAndUnavailableNeverSend) {
  Build(false);
  const auto disabled = provider->WriteOutput(signal("component.a"), true, {});
  EXPECT_EQ(disabled.outcome, core::ActionOutcome::kFailed);
  EXPECT_EQ(disabled.error->code, "io_output_not_submitted");
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto unavailable =
      provider->WriteOutput(signal("component.a"), true, {});
  EXPECT_EQ(unavailable.outcome, core::ActionOutcome::kFailed);
  EXPECT_EQ(unavailable.error->code, "io_output_not_submitted");
  EXPECT_EQ(provider->WriteOutput(signal("component.a", "absent"), true, {})
                .error->code,
            "io_signal_unknown");
  EXPECT_TRUE(received.empty());
}

TEST_F(RosBooleanOutputProviderTest, ServiceAnswerIsTheOutcome) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto accepted = Write(*provider, signal("component.a"), false);
  EXPECT_EQ(accepted.outcome, core::ActionOutcome::kSucceeded);
  EXPECT_FALSE(accepted.error);
  EXPECT_EQ(received, std::vector<bool>{false});
  accept = false;
  const auto refused = Write(*provider, signal("component.a", "alias"), true);
  EXPECT_EQ(refused.outcome, core::ActionOutcome::kFailed);
  EXPECT_EQ(refused.error->code, "io_output_source_rejected");
  EXPECT_EQ(received, (std::vector<bool>{false, true}));
}

TEST_F(RosBooleanOutputProviderTest, StopWithoutAnswerIsUnknownAndNeverResent) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  std::stop_source stop;
  stop.request_stop();
  const auto unknown =
      Write(*provider, signal("component.a"), true, false, stop.get_token());
  EXPECT_EQ(unknown.outcome, core::ActionOutcome::kUnknown);
  EXPECT_EQ(unknown.error->code, "component_action_interrupted_by_stop");
  // The request may still arrive; nothing sends it a second time.
  SpinUntil([&] { return !received.empty(); });
  EXPECT_LE(received.size(), 1U);
}

TEST_F(RosBooleanOutputProviderTest, NoAnswerWithinTheAuthoredAckTimeoutIsUnknown) {
  // The device's `output_source_ack_timeout_s` bounds the
  // answer (0.5 s here), not a fixed cap.
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto started = std::chrono::steady_clock::now();
  const auto unknown = Write(*provider, signal("component.a"), true, false);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  EXPECT_EQ(unknown.outcome, core::ActionOutcome::kUnknown);
  EXPECT_EQ(unknown.error->code, "io_output_acceptance_unknown");
  auto unbounded = channel();
  unbounded.ack_timeout_ns = 0;
  EXPECT_FALSE(CreateFixtureOutput(node, unbounded).ok());
}

TEST_F(RosBooleanOutputProviderTest,
       SharedChannelAliasesKeepDistinctActualDomainEnableFences) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  EXPECT_EQ(Write(*provider, signal("component.a", "alias"), true).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(provider->WriteOutput(signal("component.b"), true, {}).outcome,
            core::ActionOutcome::kFailed);
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2, owner("b")).ok());
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3, owner("a")).ok());
  EXPECT_EQ(provider->WriteOutput(signal("component.a"), true, {}).outcome,
            core::ActionOutcome::kFailed);
  EXPECT_EQ(Write(*provider, signal("component.b"), false).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(received, (std::vector<bool>{true, false}));
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), 2, owner("a")).ok());
}

TEST_F(RosBooleanOutputProviderTest,
       ShutdownAndTerminalOccurrenceFenceNewWrites) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2, owner("b")).ok());
  ASSERT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_FALSE(
      provider->ShutdownOccurrence(occurrence("foreign"), owner("foreign"))
          .ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3, owner("a")).ok());
  EXPECT_EQ(provider->WriteOutput(signal("component.a"), true, {}).outcome,
            core::ActionOutcome::kFailed);
  EXPECT_EQ(Write(*provider, signal("component.b"), true).outcome,
            core::ActionOutcome::kSucceeded);
  provider->Shutdown();
  EXPECT_FALSE(provider->ServiceReady());
  EXPECT_EQ(provider->WriteOutput(signal("component.b"), false, {}).outcome,
            core::ActionOutcome::kFailed);
  EXPECT_EQ(received, std::vector<bool>{true});
}

TEST_F(RosBooleanOutputProviderTest,
       AliasReplacementPreservesRetainedGatesAndRefusesStaleProof) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto predecessor = channel().aliases;
  auto candidate = predecessor;
  const auto added_owner = std::make_shared<const int>(4);
  candidate.push_back(
      {signal("component.c"), occurrence("c"), 1U, added_owner});
  EXPECT_FALSE(provider
                   ->RebindAliases({}, candidate,
                                   core::ActiveBundlePublicationGeneration(2U))
                   .ok());
  EXPECT_FALSE(provider
                   ->RebindAliases(identities(predecessor), candidate,
                                   core::ActiveBundlePublicationGeneration(1U))
                   .ok());
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(predecessor), candidate,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(predecessor), candidate,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  EXPECT_EQ(provider->ReadAliasState().aliases.size(), 4U);
  EXPECT_EQ(Write(*provider, signal("component.a"), true).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(provider->WriteOutput(signal("component.c"), true, {}).outcome,
            core::ActionOutcome::kFailed);
  ASSERT_TRUE(provider->Resume(occurrence("c"), 2, added_owner).ok());
  EXPECT_EQ(Write(*provider, signal("component.c"), false).outcome,
            core::ActionOutcome::kSucceeded);
}

TEST_F(RosBooleanOutputProviderTest,
       GpioWholeTopicCarriesKnownChannelsAcrossGroupsOnlyOnExplicitWrite) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto made_a =
      CreateFixtureOutput(node, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(made_a.ok());
  provider = std::move(made_a.value());
  auto made_b = CreateFixtureOutput(
      node, gpio_channel("b", "group.b", "two"), provider->gpio_topic());
  ASSERT_TRUE(made_b.ok());
  auto other = std::move(made_b.value());
  ASSERT_TRUE(
      SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  EXPECT_EQ(provider->WriteOutput(signal("component.a"), false, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  EXPECT_EQ(gpio_values(messages.back()),
            (GpioValues{{"group.a", {{"one", 0.0}}}}));
  EXPECT_EQ(other->WriteOutput(signal("component.b"), true, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
  EXPECT_EQ(
      gpio_values(messages.back()),
      (GpioValues{{"group.a", {{"one", 0.0}}}, {"group.b", {{"two", 1.0}}}}));
  // A disabled member's value is left out of the next whole-topic message.
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_EQ(other->WriteOutput(signal("component.b"), false, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 3U; }));
  EXPECT_EQ(gpio_values(messages.back()),
            (GpioValues{{"group.b", {{"two", 0.0}}}}));
  // Resume and rebind are not resend policies.
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  auto retained = gpio_channel("a", "group.a", "one");
  retained.aliases.front().prepared_driver_generation = 4U;
  ASSERT_TRUE(provider
                  ->RebindAliases(provider->ReadAliasState().aliases,
                                  retained.aliases,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  EXPECT_EQ(messages.size(), 3U);
  other->Shutdown();
}

TEST_F(RosBooleanOutputProviderTest, ConcurrentGpioWritesPreservePublishedAggregate) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto a = CreateFixtureOutput(node, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(a.ok());
  provider = std::move(a.value());
  auto b = CreateFixtureOutput(node, gpio_channel("b", "group.b", "two"),
                                             provider->gpio_topic());
  ASSERT_TRUE(b.ok());
  auto other = std::move(b.value());
  ASSERT_TRUE(SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  const auto subscription_qos = subscriber->get_actual_qos().get_rmw_qos_profile();
  const auto publishers = server_node->get_publishers_info_by_topic("/test/group_commands");
  ASSERT_FALSE(publishers.empty());
  const auto publisher_qos = publishers.front().qos_profile().get_rmw_qos_profile();
  std::cout << "GPIO_QOS subscription_history=" << subscription_qos.history
            << " subscription_depth=" << subscription_qos.depth
            << " subscription_reliability=" << subscription_qos.reliability
            << " publisher_history=" << publisher_qos.history
            << " publisher_depth=" << publisher_qos.depth
            << " publisher_reliability=" << publisher_qos.reliability << std::endl;
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  auto first = std::async(std::launch::async, [&] {
    return provider->WriteOutput(signal("component.a"), true, {}).outcome;
  });
  auto second = std::async(std::launch::async, [&] {
    return other->WriteOutput(signal("component.b"), true, {}).outcome;
  });
  EXPECT_EQ(first.get(), core::ActionOutcome::kSucceeded);
  EXPECT_EQ(second.get(), core::ActionOutcome::kSucceeded);
  const GpioValues final_values{{"group.a", {{"one", 1.0}}}, {"group.b", {{"two", 1.0}}}};
  ASSERT_TRUE(SpinUntil([&] {
    return !messages.empty() && gpio_values(messages.back()) == final_values;
  }));
  bool seen_a = false, seen_b = false;
  for (const auto &message : messages) {
    const auto values = gpio_values(message);
    const bool has_a = values.contains("group.a");
    const bool has_b = values.contains("group.b");
    EXPECT_FALSE(seen_a && !has_a);
    EXPECT_FALSE(seen_b && !has_b);
    if (has_a) { EXPECT_EQ(values.at("group.a").at("one"), 1.0); }
    if (has_b) { EXPECT_EQ(values.at("group.b").at("two"), 1.0); }
    seen_a = seen_a || has_a;
    seen_b = seen_b || has_b;
  }
  EXPECT_TRUE(seen_a && seen_b);
}

TEST_F(RosBooleanOutputProviderTest, GpioDisableAndWritesCompleteWithoutLockInversion) {
  auto made = CreateFixtureOutput(node, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  auto writer = std::async(std::launch::async, [&] {
    for (int i = 0; i < 100; ++i)
      (void)provider->WriteOutput(signal("component.a"), (i % 2) != 0, {});
  });
  auto disabler = std::async(std::launch::async, [&] {
    return provider->Pause(occurrence("a"), 3U, owner("a")).ok();
  });
  EXPECT_TRUE(disabler.get());
  writer.get();
  EXPECT_EQ(provider->WriteOutput(signal("component.a"), true, {}).outcome,
            core::ActionOutcome::kFailed);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  EXPECT_EQ(provider->WriteOutput(signal("component.a"), true, {}).outcome,
            core::ActionOutcome::kSucceeded);
}

#ifdef MANYFORGE_LEAF_TIMING
TEST_F(RosBooleanOutputProviderTest, ServiceDisableDoesNotWaitForAdmittedSend) {
  namespace timing = ros2_control_execution::detail;
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  timing::io_service_admitted_entered.store(false);
  timing::release_io_service_send.store(false);
  timing::block_io_service_send.store(true);
  auto first = std::async(std::launch::async, [&] {
    return provider->WriteOutput(signal("component.a"), true, {}).outcome;
  });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!timing::io_service_admitted_entered.load() && std::chrono::steady_clock::now() < until)
    std::this_thread::yield();
  const bool entered = timing::io_service_admitted_entered.load();
  auto pause_future = std::async(std::launch::async, [&] {
    return provider->Pause(occurrence("a"), 3U, owner("a"));
  });
  const bool pause_ready = pause_future.wait_for(200ms) == std::future_status::ready;
  if (!pause_ready) timing::release_io_service_send.store(true);
  const auto pause = pause_future.get();
  const auto refused = provider->WriteOutput(signal("component.a"), false, {});
  timing::release_io_service_send.store(true);
  timing::block_io_service_send.store(false);
  const bool finished = SpinUntil([&] { return first.wait_for(0s) == std::future_status::ready; });
  const auto outcome = first.get();
  EXPECT_TRUE(entered);
  EXPECT_TRUE(pause_ready);
  EXPECT_TRUE(pause.ok());
  EXPECT_EQ(refused.outcome, core::ActionOutcome::kFailed);
  EXPECT_TRUE(finished);
  EXPECT_EQ(outcome, core::ActionOutcome::kSucceeded);
  EXPECT_EQ(received, (std::vector<bool>{true}));
}

TEST_F(RosBooleanOutputProviderTest, ServiceLeafWaitPastDeadlineIsKnownUnsent) {
  namespace timing = ros2_control_execution::detail;
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  timing::io_service_admitted_entered.store(false);
  timing::release_io_service_send.store(false);
  timing::block_io_service_send.store(true);
  auto first = std::async(std::launch::async, [&] {
    return provider->WriteOutput(signal("component.a"), true, {});
  });
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!timing::io_service_admitted_entered.load() && std::chrono::steady_clock::now() < until)
    std::this_thread::yield();
  const bool entered = timing::io_service_admitted_entered.load();
  timing::io_service_waiting.store(false);
  auto second = std::async(std::launch::async, [&] {
    return provider->WriteOutput(signal("component.a"), false, {});
  });
  const auto waiting_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!timing::io_service_waiting.load() && std::chrono::steady_clock::now() < waiting_until)
    std::this_thread::yield();
  const bool waiting = timing::io_service_waiting.load();
  std::this_thread::sleep_for(600ms); // authored acknowledgment bound is 500 ms
  timing::release_io_service_send.store(true);
  timing::block_io_service_send.store(false);
  (void)SpinUntil([&] { return first.wait_for(0s) == std::future_status::ready &&
                              second.wait_for(0s) == std::future_status::ready &&
                              received.size() == 1U; });
  const auto first_result = first.get();
  const auto second_result = second.get();
  EXPECT_TRUE(entered);
  EXPECT_TRUE(waiting);
  EXPECT_EQ(second_result.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(second_result.error.has_value());
  EXPECT_EQ(second_result.error->code, "io_output_not_submitted");
  EXPECT_EQ(received.size(), 1U);
  (void)first_result; // its deliberately delayed response may be unknown
}

TEST_F(RosBooleanOutputProviderTest, GpioLeafTimingConcurrentWriters) {
  namespace timing = ros2_control_execution::detail;
  auto a = CreateFixtureOutput(node, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(a.ok());
  provider = std::move(a.value());
  auto b = CreateFixtureOutput(node, gpio_channel("b", "group.b", "two"),
                                             provider->gpio_topic());
  ASSERT_TRUE(b.ok());
  auto other = std::move(b.value());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  timing::leaf_sample_count.store(0);
  struct Watchdog {
    std::mutex mutex;
    std::condition_variable wake;
    bool done{false};
    std::array<int, 2> iteration{};
    std::array<std::int64_t, 2> deadline{};
    std::thread thread;
    static std::int64_t Now() {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    Watchdog() {
      for (auto &end : deadline) end = Now() + 3'000'000'000LL;
      thread = std::thread([this] {
        std::unique_lock lock(mutex);
        while (!wake.wait_for(lock, 10ms, [this] { return done; })) {
          for (std::size_t i = 0; i < deadline.size(); ++i) {
            const auto end = deadline[i];
            if (end != 0 && Now() > end) {
              std::fprintf(stderr, "GpioLeafTimingConcurrentWriters watchdog writer=%zu iteration=%d\n",
                           i, iteration[i]);
              std::fflush(stderr);
              std::abort(); // test-only: preserve a core for the hung iteration
            }
          }
        }
      });
    }
    ~Watchdog() {
      { std::lock_guard lock(mutex); done = true; }
      wake.notify_all();
      thread.join();
    }
  } watchdog;
  auto run = [&](std::size_t which, RosBooleanOutputProvider &p, const core::IoSignalId &id) {
    for (int i = 0; i < 1000; ++i) {
      {
        std::lock_guard lock(watchdog.mutex);
        watchdog.iteration[which] = i;
        watchdog.deadline[which] = Watchdog::Now() + 3'000'000'000LL;
      }
      if (p.WriteOutput(id, (i % 2) != 0, {}).outcome != core::ActionOutcome::kSucceeded) {
        std::lock_guard lock(watchdog.mutex);
        watchdog.deadline[which] = 0;
        return false;
      }
    }
    { std::lock_guard lock(watchdog.mutex); watchdog.deadline[which] = 0; }
    return true;
  };
  auto first = std::async(std::launch::async, [&] { return run(0, *provider, signal("component.a")); });
  auto second = std::async(std::launch::async, [&] { return run(1, *other, signal("component.b")); });
  EXPECT_TRUE(first.get());
  EXPECT_TRUE(second.get());
  const auto count = timing::leaf_sample_count.load();
  ASSERT_LE(count, timing::leaf_samples.size());
  std::vector<std::int64_t> waits, holds;
  for (std::size_t i = 0; i < count; ++i) {
    const auto &sample = timing::leaf_samples[i];
    if (sample.entity != timing::LeafEntity::kGpioPublish) continue;
    ASSERT_TRUE(sample.acquired);
    waits.push_back(sample.wait_ns);
    holds.push_back(sample.hold_ns);
  }
  ASSERT_EQ(waits.size(), 2000U);
  std::sort(waits.begin(), waits.end());
  std::sort(holds.begin(), holds.end());
  const auto p99 = (waits.size() * 99 + 99) / 100 - 1;
  std::cout << "LEAF_TIMING gpio_publish count=" << waits.size()
            << " wait_ns_p50=" << waits[waits.size()/2] << " wait_ns_p99=" << waits[p99]
            << " wait_ns_max=" << waits.back() << " hold_ns_p50=" << holds[holds.size()/2]
            << " hold_ns_p99=" << holds[p99] << " hold_ns_max=" << holds.back() << std::endl;
}

TEST_F(RosBooleanOutputProviderTest, GpioRecoveryCopiesInflightAndFencesQueuedWrite) {
  namespace timing = ros2_control_execution::detail;
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto made = CreateFixtureOutput(node, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  auto other_result = CreateFixtureOutput(
      node, gpio_channel("b", "group.b", "two"), provider->gpio_topic());
  ASSERT_TRUE(other_result.ok());
  auto other = std::move(other_result.value());
  ASSERT_TRUE(SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  const auto snapshot_a = provider->CaptureTransportRecovery();
  const auto snapshot_b = other->CaptureTransportRecovery();
  auto replacement = ros2_control_execution::Ros2TransportResources::create(
      "gpio_cut_replacement", "", 1U, [this] { return context; });
  timing::gpio_publish_entered.store(false);
  timing::gpio_recovery_closed.store(false);
  timing::release_gpio_publish.store(false);
  timing::block_gpio_publish.store(true);
  auto first = std::async(std::launch::async, [&] {
    return provider->WriteOutput(signal("component.a"), true, {}).outcome;
  });
  const auto wait = [](auto &flag) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!flag.load() && std::chrono::steady_clock::now() < until)
      std::this_thread::yield();
    return flag.load();
  };
  const bool entered = wait(timing::gpio_publish_entered);
  auto stage_future = std::async(std::launch::async, [&] {
    return RosBooleanOutputProvider::StageGpioTransportRecovery(provider->gpio_topic(), replacement);
  });
  const bool closed = wait(timing::gpio_recovery_closed);
  auto queued = std::async(std::launch::async, [&] {
    return other->WriteOutput(signal("component.b"), true, {}).outcome;
  });
  timing::release_gpio_publish.store(true);
  timing::block_gpio_publish.store(false);
  const auto first_outcome = first.get();
  const auto queued_outcome = queued.get();
  auto staged = stage_future.get();
  EXPECT_TRUE(entered);
  EXPECT_TRUE(closed);
  EXPECT_EQ(first_outcome, core::ActionOutcome::kSucceeded);
  EXPECT_EQ(queued_outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(staged.ok());
  auto stage_a = RosBooleanOutputProvider::StageTransportRecovery(snapshot_a, replacement, staged.value());
  auto stage_b = RosBooleanOutputProvider::StageTransportRecovery(snapshot_b, replacement, staged.value());
  ASSERT_TRUE(stage_a.ok());
  ASSERT_TRUE(stage_b.ok());
  RosBooleanOutputProvider::CommitGpioTransportRecovery(*staged.value());
  provider->CommitTransportRecovery(*stage_a.value());
  other->CommitTransportRecovery(*stage_b.value());
  replacement->start();
  EXPECT_EQ(other->WriteOutput(signal("component.b"), false, {}).outcome,
            core::ActionOutcome::kSucceeded);
  const GpioValues final_values{{"group.a", {{"one", 1.0}}}, {"group.b", {{"two", 0.0}}}};
  ASSERT_TRUE(SpinUntil([&] {
    return !messages.empty() && gpio_values(messages.back()) == final_values;
  }));
  bool seen_a = false, seen_b = false;
  for (const auto &message : messages) {
    const auto values = gpio_values(message);
    const bool has_a = values.contains("group.a");
    const bool has_b = values.contains("group.b");
    EXPECT_FALSE(seen_a && !has_a);
    EXPECT_FALSE(seen_b && !has_b);
    if (has_a) { EXPECT_EQ(values.at("group.a").at("one"), 1.0); }
    if (has_b) { EXPECT_EQ(values.at("group.b").at("two"), 0.0); }
    seen_a = seen_a || has_a;
    seen_b = seen_b || has_b;
  }
  EXPECT_TRUE(seen_a && seen_b);
  other->Shutdown();
  provider->Shutdown();
  replacement->quiesce();
}
#endif

TEST_F(RosBooleanOutputProviderTest, OwnedGpioPinSurvivesForeignDropAndCrossNodeWrite) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto transport = ros2_control_execution::Ros2TransportResources::create(
      "manyforge_gpio_owned_test", "", 1U, [this] { return context; });
  auto first = RosBooleanOutputProvider::Create(
      transport->node(), gpio_channel("a", "group.a", "one"), {}, transport);
  ASSERT_TRUE(first.ok());
  auto first_provider = std::move(first.value());
  auto pin = first_provider->gpio_topic();
  auto second = CreateFixtureOutput(
      node, gpio_channel("b", "group.b", "two"), pin);
  ASSERT_TRUE(second.ok());
  auto second_provider = std::move(second.value());
  ASSERT_TRUE(second_provider->Resume(occurrence("b"), 2U, owner("b")).ok());
  transport->start();
  ASSERT_TRUE(SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  auto drop = std::async(std::launch::async, [&] { first_provider.reset(); });
  drop.get();
  EXPECT_EQ(second_provider->WriteOutput(signal("component.b"), true, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  EXPECT_EQ(gpio_values(messages.back()), (GpioValues{{"group.b", {{"two", 1.0}}}}));
  transport->quiesce();
  EXPECT_EQ(second_provider->WriteOutput(signal("component.b"), false, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
  EXPECT_EQ(gpio_values(messages.back()), (GpioValues{{"group.b", {{"two", 0.0}}}}));
  second_provider.reset();
  pin.reset();
}

TEST_F(RosBooleanOutputProviderTest, AddressMustBeAbsoluteAndUnremapped) {
  auto relative = channel();
  relative.address = RosSetBoolOutputAddress{"relative"};
  EXPECT_FALSE(CreateFixtureOutput(node, relative).ok());
  auto empty = channel();
  empty.aliases.clear();
  EXPECT_FALSE(CreateFixtureOutput(node, empty).ok());
  EXPECT_FALSE(CreateFixtureOutput(
                   node, gpio_channel("a", "", "one"))
                   .ok());
  auto remap_context = std::make_shared<rclcpp::Context>();
  const char *arguments[] = {"output_test", "--ros-args", "-r",
                             "/test/manyforge_set_bool:=/test/other_set_bool"};
  remap_context->init(4, arguments);
  auto remapped = ros2_control_execution::Ros2TransportResources::create(
      "remapped_output", "", 1U, [remap_context] { return remap_context; });
  const auto rejected = RosBooleanOutputProvider::Create(remapped->node(), channel(), {}, remapped);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "ros_output_service_remapped");
  remapped->quiesce();
  remap_context->shutdown("remapping checked");
}

} // namespace
TEST_F(RosBooleanOutputProviderTest,
       GpioTopicPinRequiresActualContextAddressAndUniqueCanonicalChannel) {
  auto configuration = gpio_channel("a", "group.a", "one");
  auto made = CreateFixtureOutput(node, configuration);
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  EXPECT_FALSE(CreateFixtureOutput(node, configuration,
                                                provider->gpio_topic())
                   .ok());
  auto foreign_context = std::make_shared<rclcpp::Context>();
  foreign_context->init(0, nullptr);
  auto foreign_owner = ros2_control_execution::Ros2TransportResources::create(
      "foreign_gpio", "", 1U, [foreign_context] { return foreign_context; });
  const auto foreign = RosBooleanOutputProvider::Create(
      foreign_owner->node(), gpio_channel("b", "group.b", "two"),
      provider->gpio_topic(), foreign_owner);
  ASSERT_FALSE(foreign.ok());
  EXPECT_EQ(foreign.error().code, "io_ros_output_setup_failed");
  foreign_owner->quiesce();
  foreign_context->shutdown("GPIO context mismatch checked");
  auto duplicate = gpio_channel("b", "group.a", "one");
  EXPECT_FALSE(CreateFixtureOutput(node, duplicate,
                                                provider->gpio_topic())
                   .ok());
  auto wrong_topic = gpio_channel("b", "group.b", "two", "/test/another_group");
  EXPECT_FALSE(CreateFixtureOutput(node, wrong_topic,
                                                provider->gpio_topic())
                   .ok());
  EXPECT_FALSE(CreateFixtureOutput(node, channel(),
                                                provider->gpio_topic())
                   .ok());
}

TEST_F(RosBooleanOutputProviderTest,
       SetBoolRecoveryRetainsPausedAliasesWithoutReplay) {
  Build();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  const auto aliases = provider->ReadAliasState();
  const auto snapshot = provider->CaptureTransportRecovery();
  EXPECT_FALSE(
      RosBooleanOutputProvider::StageTransportRecovery(snapshot, {}).ok());
  EXPECT_TRUE(provider->MatchesTransportRecovery(*snapshot));
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "setbool_recovery_new", "", 1U, [this] { return context; });
  auto staged =
      RosBooleanOutputProvider::StageTransportRecovery(snapshot, replacement);
  ASSERT_TRUE(staged.ok());
  EXPECT_TRUE(received.empty());
  provider->CommitTransportRecovery(*staged.value());
  EXPECT_FALSE(provider->MatchesTransportRecovery(*snapshot));
  EXPECT_EQ(provider->ReadAliasState().aliases.size(), aliases.aliases.size());
  EXPECT_EQ(provider->WriteOutput(signal("component.a"), true, {}).outcome,
            core::ActionOutcome::kFailed);
  EXPECT_TRUE(received.empty());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  replacement->start();
  ASSERT_TRUE(SpinUntil([&] { return provider->ServiceReady(); }));
  EXPECT_EQ(Write(*provider, signal("component.a", "alias"), true).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(received, (std::vector<bool>{true}));
  provider->Shutdown();
  replacement->quiesce();
}

TEST_F(RosBooleanOutputProviderTest,
       SharedGpioRecoveryRetainsAliasesAndWrittenModel) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto made = CreateFixtureOutput(
      node, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  auto pin = provider->gpio_topic();
  auto made_other = CreateFixtureOutput(
      node, gpio_channel("b", "group.b", "two"), pin);
  ASSERT_TRUE(made_other.ok());
  auto other = std::move(made_other.value());
  ASSERT_TRUE(
      SpinUntil([&] { return subscriber->get_publisher_count() >= 1U; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  ASSERT_EQ(provider->WriteOutput(signal("component.a"), true, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  ASSERT_TRUE(other->Pause(occurrence("b"), 3U, owner("b")).ok());
  auto first = provider->CaptureTransportRecovery();
  auto second = other->CaptureTransportRecovery();
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "gpio_recovery_new", "", 1U, [this] { return context; });
  auto topic_stage =
      RosBooleanOutputProvider::StageGpioTransportRecovery(pin, replacement);
  ASSERT_TRUE(topic_stage.ok());
  auto first_stage = RosBooleanOutputProvider::StageTransportRecovery(
      first, replacement, topic_stage.value());
  auto second_stage = RosBooleanOutputProvider::StageTransportRecovery(
      second, replacement, topic_stage.value());
  ASSERT_TRUE(first_stage.ok());
  ASSERT_TRUE(second_stage.ok());
  ASSERT_TRUE(provider->MatchesTransportRecovery(*first));
  ASSERT_TRUE(other->MatchesTransportRecovery(*second));
  EXPECT_EQ(messages.size(), 1U);
  RosBooleanOutputProvider::CommitGpioTransportRecovery(*topic_stage.value());
  provider->CommitTransportRecovery(*first_stage.value());
  other->CommitTransportRecovery(*second_stage.value());
  EXPECT_FALSE(provider->MatchesTransportRecovery(*first));
  EXPECT_EQ(provider->gpio_topic(), pin);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 4U, owner("b")).ok());
  replacement->start();
  ASSERT_TRUE(
      SpinUntil([&] { return subscriber->get_publisher_count() >= 2U; }));
  ASSERT_EQ(other->WriteOutput(signal("component.b"), false, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
  EXPECT_EQ(
      gpio_values(messages.back()),
      (GpioValues{{"group.a", {{"one", 1.0}}}, {"group.b", {{"two", 0.0}}}}));
  other->Shutdown();
  provider->Shutdown();
  replacement->quiesce();
}

} // namespace manyforge::roboplan_backend
