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

#include <algorithm>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <map>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <stop_token>
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
    node =
        std::make_shared<rclcpp::Node>("manyforge_output_client_test", options);
    server_node =
        std::make_shared<rclcpp::Node>("manyforge_output_server_test", options);
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    client_executor =
        std::make_unique<rclcpp::executors::SingleThreadedExecutor>(
            executor_options);
    server_executor =
        std::make_unique<rclcpp::executors::SingleThreadedExecutor>(
            executor_options);
    client_executor->add_node(node);
    server_executor->add_node(server_node);
  }
  void TearDown() override {
    if (provider)
      provider->Shutdown();
    client_executor->cancel();
    server_executor->cancel();
    client_executor->remove_node(node);
    server_executor->remove_node(server_node);
    provider.reset();
    service.reset();
    client_executor.reset();
    server_executor.reset();
    node.reset();
    server_node.reset();
    context->shutdown("boolean output fixture complete");
  }
  bool SpinUntil(const std::function<bool()> &condition,
                 bool spin_server = true) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      if (spin_server)
        server_executor->spin_some();
      client_executor->spin_once(5ms);
    }
    return condition();
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
    auto created = RosBooleanOutputProvider::Create(node, channel());
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
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> client_executor,
      server_executor;
  rclcpp::Service<Service>::SharedPtr service;
  std::unique_ptr<RosBooleanOutputProvider> provider;
  std::vector<bool> received;
  bool accept = true;
};

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
  // CONTRACTS §7: the device's `output_source_ack_timeout_s` bounds the
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
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, unbounded).ok());
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
      RosBooleanOutputProvider::Create(node, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(made_a.ok());
  provider = std::move(made_a.value());
  auto made_b = RosBooleanOutputProvider::Create(
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

TEST_F(RosBooleanOutputProviderTest, AddressMustBeAbsoluteAndUnremapped) {
  auto relative = channel();
  relative.address = RosSetBoolOutputAddress{"relative"};
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, relative).ok());
  auto empty = channel();
  empty.aliases.clear();
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, empty).ok());
  EXPECT_FALSE(RosBooleanOutputProvider::Create(
                   node, gpio_channel("a", "", "one"))
                   .ok());
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  options.arguments({"--ros-args", "-r",
                     "/test/manyforge_set_bool:=/test/other_set_bool"});
  auto remapped = std::make_shared<rclcpp::Node>("remapped_output", options);
  EXPECT_FALSE(RosBooleanOutputProvider::Create(remapped, channel()).ok());
}

} // namespace
TEST_F(RosBooleanOutputProviderTest,
       GpioTopicPinRequiresActualContextAddressAndUniqueCanonicalChannel) {
  auto configuration = gpio_channel("a", "group.a", "one");
  auto made = RosBooleanOutputProvider::Create(node, configuration);
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, configuration,
                                                provider->gpio_topic())
                   .ok());
  auto foreign_context = std::make_shared<rclcpp::Context>();
  foreign_context->init(0, nullptr);
  rclcpp::NodeOptions foreign_options;
  foreign_options.context(foreign_context);
  foreign_options.use_global_arguments(false);
  auto foreign_node =
      std::make_shared<rclcpp::Node>("foreign_gpio", foreign_options);
  EXPECT_FALSE(RosBooleanOutputProvider::Create(
                   foreign_node, gpio_channel("b", "group.b", "two"),
                   provider->gpio_topic())
                   .ok());
  foreign_node.reset();
  foreign_context->shutdown("GPIO context mismatch checked");
  auto duplicate = gpio_channel("b", "group.a", "one");
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, duplicate,
                                                provider->gpio_topic())
                   .ok());
  auto wrong_topic = gpio_channel("b", "group.b", "two", "/test/another_group");
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, wrong_topic,
                                                provider->gpio_topic())
                   .ok());
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, channel(),
                                                provider->gpio_topic())
                   .ok());
}

} // namespace manyforge::roboplan_backend
