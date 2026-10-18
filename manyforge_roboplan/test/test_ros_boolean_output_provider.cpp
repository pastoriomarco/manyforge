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

#include <manyforge_roboplan/ros_boolean_output_provider.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_core/io_output_participant_engine.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <map>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <stdexcept>
#include <thread>
#include <ur_msgs/srv/set_io.hpp>
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
          core::ActiveBundlePublicationGeneration(1U)};
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

core::IoOutputWriteRequestDTO request(std::uint64_t number, bool value = true,
                                      std::string component = "component.a",
                                      std::string provider = "a") {
  return {{{core::OperationId("operation." + std::to_string(number)),
            core::OperationRevision(1U),
            core::OperationParticipantId("participant"),
            core::ControllerGoalId("goal." + std::to_string(number)),
            core::ControllerGoalGeneration(1U)},
           kDigest,
           core::ComponentLifecycleGeneration(7U),
           occurrence(std::move(provider))},
          signal(std::move(component)),
          value,
          1000};
}

core::ResolvedPreparedCapabilityCommandExecutionRequestDTO
submitted_engine_request(const RosBooleanOutputAlias &alias,
                         const core::ResourceId &resource) {
  const auto schema = core::builtin_io_signal_write_command_schema();
  const auto *capability =
      core::find_builtin_capability_definition("io.gpio.set_output");
  if (!capability)
    throw std::logic_error("missing output capability fixture");
  const core::CapabilityId capability_id("io.gpio.set_output");
  const core::CapabilityDefinitionRevision capability_revision(
      std::string(capability->capability_definition_revision));
  const core::IoSignalDescriptorDTO descriptor{
      alias.signal_id,
      core::InterruptSourceId("source.gpio"),
      core::IoSignalValueType::kBoolean,
      core::IoSignalDirection::kOutput,
      core::IoSignalUpdateMode::kChangeOnly,
      core::IoTransitionCapability::kSampledOnly,
      {core::IoFreshnessMode::kProviderValidity, std::nullopt},
      resource,
      std::nullopt};
  const auto digest = core::ComputeIoSignalDescriptorDigest(descriptor);
  if (!digest.ok())
    throw std::logic_error(digest.error().message);
  const auto &provider = alias.provider;
  const core::ResourceRoleId role("output");
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.output"),
      core::ProgramRevision(kDigest.value()),
      core::ProgramBindingId("binding.output"),
      core::RetainedBundleId("bundle.output"),
      core::GraphRevision("graph.output"),
      core::ComponentRoleId("device"),
      core::TargetRoleId("gpio"),
      alias.signal_id.controllable_component_id,
      core::ComponentTypeId("test.io"),
      core::ComponentTypeRevision(kDigest.value()),
      core::CommandTargetId("gpio"),
      capability_id,
      capability_revision,
      core::ProgramCommandSchemaRefDTO{schema.command_schema_id,
                                       schema.command_schema_revision},
      {},
      {},
      {},
      {},
      {},
      std::nullopt,
      core::RuntimeCapabilityProviderPathDTO{
          capability_id, capability_revision,
          core::CapabilityProviderBindingId("provider.output"),
          core::CapabilityProviderBindingRevision(kDigest.value()),
          provider.provider_instance_id, provider.provider_manifest_id,
          provider.provider_manifest_revision,
          provider.provider_runtime_unit_id, provider.provider_runtime_epoch},
      {},
      {{provider.provider_runtime_unit_id, provider.provider_runtime_epoch}},
      {},
      core::RuntimeIoOutputBindingDTO{
          role, descriptor, digest.value(),
          core::ComponentLifecycleGeneration(3U), provider, kDigest,
          core::IoOutputCompletionPolicyDTO(core::IoOutputSubmittedDTO{})}};
  auto binding = core::verify_runtime_participant_binding(std::move(value));
  if (!binding.ok())
    throw std::logic_error(binding.error().message);
  const core::CapabilityCommandIntentDTO command{
      schema.command_schema_id,
      schema.command_schema_revision,
      core::IoSignalWriteCommandValueDTO{role, true},
      std::nullopt,
      {}};
  auto made = core::MakePreparedIoOutputParticipant(
      core::OperationId("operation.output"), core::OperationRevision(1U),
      core::OperationParticipantId("participant.output"),
      core::ProgramRunId("run.output"), command, binding.value());
  if (!made.ok())
    throw std::logic_error(made.error().message);
  auto product = std::make_shared<const core::PreparedOperationParticipantDTO>(
      std::move(made.value()));
  const auto &output =
      std::get<core::PreparedIoOutputCommandDTO>(product->payload);
  return {
      product, binding.value(),
      core::ResolvedPreparedIoOutputCommandAuthorityDTO{
          output.signal_id, output.output_resource, output.command_value.value,
          product->provider()->lifecycle_provider, output.completion_policy}};
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
    auto created = RosBooleanOutputProvider::Create(node, clock, channel());
    ASSERT_TRUE(created.ok()) << created.error().message;
    provider = std::move(created.value());
    if (with_service) {
      ASSERT_TRUE(SpinUntil([&] { return provider->ServiceReady(); }));
    }
  }
  core::IoOutputEvidence
  Evidence(const core::IoOutputCommandIdentityDTO &identity) {
    return provider->ObserveOutputResult(identity).value().evidence;
  }

  core::FakeClock clock;
  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr node, server_node;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> client_executor,
      server_executor;
  rclcpp::Service<Service>::SharedPtr service;
  std::unique_ptr<RosBooleanOutputProvider> provider;
  std::vector<bool> received;
  bool accept = true;
};

TEST_F(RosBooleanOutputProviderTest, DisabledAndUnavailableNeverSendOrRetry) {
  Build(false);
  ASSERT_NE(provider, nullptr);
  const auto first = request(1);
  auto refused = provider->SubmitOutput(first);
  ASSERT_TRUE(refused.ok());
  EXPECT_EQ(refused.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(refused.value().error->details.at("reason"),
            "io_output_driver_disabled");
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  EXPECT_EQ(provider->SubmitOutput(first).value(), refused.value());
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  const auto unavailable = provider->SubmitOutput(request(2));
  ASSERT_TRUE(unavailable.ok());
  EXPECT_EQ(unavailable.value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(unavailable.value().error->details.at("reason"),
            "io_output_service_unavailable");
  EXPECT_TRUE(received.empty());
}

TEST_F(RosBooleanOutputProviderTest,
       SourceAcceptanceRequiresActualCorrelatedResponse) {
  Build();
  ASSERT_NE(provider, nullptr);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto write = request(1, false);
  clock.set(10);
  const auto submitted = provider->SubmitOutput(write);
  ASSERT_TRUE(submitted.ok());
  EXPECT_EQ(submitted.value().evidence, core::IoOutputEvidence::kSubmitted);
  EXPECT_TRUE(received.empty());
  EXPECT_EQ(provider->SubmitOutput(write).value(), submitted.value());
  clock.set(25);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(write.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  ASSERT_EQ(received.size(), 1U);
  EXPECT_FALSE(received.front());
  const auto accepted = provider->ObserveOutputResult(write.identity).value();
  EXPECT_EQ(accepted.identity, write.identity);
  EXPECT_EQ(accepted.trusted_received_at_ns, 25);
  EXPECT_FALSE(accepted.error);
  clock.set(5000);
  EXPECT_EQ(provider->SubmitOutput(write).value(), accepted);
  EXPECT_EQ(received.size(), 1U);
}

TEST_F(RosBooleanOutputProviderTest,
       RejectionIsNotReadbackAndPausePreservesPendingReply) {
  Build();
  ASSERT_NE(provider, nullptr);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  accept = false;
  const auto write = request(1);
  ASSERT_EQ(provider->SubmitOutput(write).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3, owner("a")).ok());
  clock.set(40);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(write.identity) == core::IoOutputEvidence::kSourceRejected;
  }));
  const auto rejected = provider->ObserveOutputResult(write.identity).value();
  ASSERT_TRUE(rejected.error);
  EXPECT_EQ(rejected.error->code, "io_output_source_rejected");
  EXPECT_EQ(rejected.trusted_received_at_ns, 40);
  EXPECT_EQ(provider->SubmitOutput(write).value(), rejected);
  EXPECT_EQ(received.size(), 1U);
  ASSERT_TRUE(provider->RetireOutput(write.identity).ok());
  EXPECT_EQ(provider->SubmitOutput(request(2)).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3, owner("a")).ok());
}

TEST_F(RosBooleanOutputProviderTest,
       DeadlineIsOnlyPreSendFenceAndObservationDoesNotInventTimeout) {
  Build();
  ASSERT_NE(provider, nullptr);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  auto missing = request(1);
  missing.absolute_deadline_ns.reset();
  const auto unbounded = provider->SubmitOutput(missing);
  ASSERT_TRUE(unbounded.ok());
  EXPECT_EQ(unbounded.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(unbounded.value().error);
  EXPECT_EQ(unbounded.value().error->details.at("reason"),
            "io_output_deadline_missing");
  EXPECT_EQ(provider->SubmitOutput(missing).value(), unbounded.value());
  EXPECT_TRUE(received.empty());
  ASSERT_TRUE(provider->RetireOutput(missing.identity).ok());
  auto expired = request(2);
  expired.absolute_deadline_ns = 0;
  const auto refused = provider->SubmitOutput(expired);
  ASSERT_TRUE(refused.ok());
  EXPECT_EQ(refused.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(refused.value().error->details.at("reason"),
            "io_output_deadline_expired");
  EXPECT_TRUE(received.empty());
  ASSERT_TRUE(provider->RetireOutput(expired.identity).ok());
  auto pending = request(3);
  pending.absolute_deadline_ns = 10;
  ASSERT_EQ(provider->SubmitOutput(pending).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  clock.set(100);
  EXPECT_EQ(Evidence(pending.identity), core::IoOutputEvidence::kSubmitted);
  EXPECT_EQ(provider->SubmitOutput(pending).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(pending.identity) ==
           core::IoOutputEvidence::kSourceAccepted;
  }));
  EXPECT_EQ(received.size(), 1U);
}

TEST_F(RosBooleanOutputProviderTest,
       ActiveRequestIsImmutableAndChannelAliasesHaveSeparateGates) {
  Build();
  ASSERT_NE(provider, nullptr);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  auto write = request(1);
  write.signal_id = signal("component.a", "alias");
  ASSERT_EQ(provider->SubmitOutput(write).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  auto changed = write;
  changed.value = !changed.value;
  EXPECT_FALSE(provider->SubmitOutput(changed).ok());
  changed = write;
  ++*changed.absolute_deadline_ns;
  EXPECT_FALSE(provider->SubmitOutput(changed).ok());
  changed = write;
  changed.absolute_deadline_ns.reset();
  EXPECT_FALSE(provider->SubmitOutput(changed).ok());
  changed = write;
  changed.signal_id = signal("component.a");
  EXPECT_FALSE(provider->SubmitOutput(changed).ok());
  EXPECT_FALSE(provider->SubmitOutput(request(2)).ok());
  EXPECT_FALSE(provider->RetireOutput(request(2).identity).ok());
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(write.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  ASSERT_TRUE(provider->RetireOutput(write.identity).ok());
  const auto second = request(2, false, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(second).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2, owner("b")).ok());
  EXPECT_EQ(provider->SubmitOutput(second).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(provider->RetireOutput(second.identity).ok());
  auto third = request(3, false, "component.b", "b");
  ASSERT_EQ(provider->SubmitOutput(third).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(third.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  ASSERT_EQ(received.size(), 2U);
  EXPECT_FALSE(received.back());
}

TEST_F(RosBooleanOutputProviderTest,
       RetirementFencesDelayedReplyWithoutClaimingRemoteUndo) {
  Build();
  ASSERT_NE(provider, nullptr);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto first = request(1, false);
  ASSERT_EQ(provider->SubmitOutput(first).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  EXPECT_FALSE(provider->ObserveOutputResult(first.identity).ok());
  // The remote request may still execute; retirement must never claim it was
  // undone. Deliver it before the next request and retain a different result.
  accept = false;
  ASSERT_TRUE(SpinUntil([&] { return received.size() == 1U; }));
  const auto next = request(2, true);
  accept = true;
  ASSERT_EQ(provider->SubmitOutput(next).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(next.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  EXPECT_EQ(received.size(), 2U);
  EXPECT_FALSE(provider->ObserveOutputResult(first.identity).ok());
}

TEST_F(RosBooleanOutputProviderTest,
       ShutdownIsUnknownForPendingEffectsAndNeverReopens) {
  Build();
  ASSERT_NE(provider, nullptr);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto write = request(1);
  ASSERT_EQ(provider->SubmitOutput(write).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  provider->Shutdown();
  provider->Shutdown();
  EXPECT_EQ(Evidence(write.identity),
            core::IoOutputEvidence::kAcceptanceUnknown);
  EXPECT_FALSE(provider->ServiceReady());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3, owner("a")).ok());
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(write.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  EXPECT_EQ(received.size(), 1U);
  ASSERT_TRUE(provider->RetireOutput(write.identity).ok());
  EXPECT_EQ(provider->SubmitOutput(request(2)).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
}

TEST_F(RosBooleanOutputProviderTest,
       RejectsForeignScopeStaleDriverAndMalformedCommittedIdentity) {
  Build();
  ASSERT_NE(provider, nullptr);
  auto wrong = occurrence("a");
  wrong.provider_runtime_epoch = core::RuntimeEpoch("old");
  EXPECT_FALSE(provider->Resume(wrong, 50, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(wrong, 50, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), 2, owner("a")).ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 1, owner("a")).ok());
  auto invalid = request(1);
  invalid.identity.provider = wrong;
  EXPECT_FALSE(provider->SubmitOutput(invalid).ok());
  invalid = request(1);
  invalid.identity.prepared_contract_digest = core::ContentDigest("invalid");
  EXPECT_FALSE(provider->SubmitOutput(invalid).ok());
  invalid = request(1);
  invalid.identity.command.command_generation =
      core::ControllerGoalGeneration(0);
  EXPECT_FALSE(provider->SubmitOutput(invalid).ok());
  EXPECT_TRUE(received.empty());
}

TEST_F(RosBooleanOutputProviderTest,
       ContextShutdownDoesNotProveSendFailureAndNeverResends) {
  // The actual RMW may still accept async_send_request after context shutdown.
  // Preserve that truthful Submitted fact; shutdown alone cannot prove that
  // the send threw, that the request was unsent, or that the source accepted.
  class ContextShutdownClock final : public core::IClock {
  public:
    core::TimePoint now() const override {
      if (++reads == 2U)
        shutdown();
      return 10;
    }
    mutable std::size_t reads = 0;
    std::function<void()> shutdown;
  } send_clock;
  Build();
  ASSERT_NE(provider, nullptr);
  provider.reset();
  send_clock.shutdown = [&] {
    context->shutdown("lose context at actual SetBool send");
  };
  auto created = RosBooleanOutputProvider::Create(node, send_clock, channel());
  ASSERT_TRUE(created.ok());
  auto interrupted_provider = std::move(created.value());
  ASSERT_TRUE(SpinUntil([&] { return interrupted_provider->ServiceReady(); }));
  ASSERT_TRUE(
      interrupted_provider->Resume(occurrence("a"), 2, owner("a")).ok());
  const auto write = request(1);
  const auto attempted = interrupted_provider->SubmitOutput(write);
  ASSERT_TRUE(attempted.ok());
  ASSERT_TRUE(
      attempted.value().evidence == core::IoOutputEvidence::kSubmitted ||
      attempted.value().evidence == core::IoOutputEvidence::kAcceptanceUnknown);
  if (attempted.value().evidence == core::IoOutputEvidence::kSubmitted) {
    EXPECT_FALSE(attempted.value().error);
  } else {
    ASSERT_TRUE(attempted.value().error);
    EXPECT_EQ(attempted.value().error->code, "io_output_acceptance_unknown");
  }
  EXPECT_EQ(interrupted_provider->SubmitOutput(write).value(),
            attempted.value());
  EXPECT_EQ(interrupted_provider->ObserveOutputResult(write.identity).value(),
            attempted.value());
  // The existing leaf shutdown conservatively records uncertainty, retaining
  // the exact correlation. It does not cancel/undo a possible remote effect.
  interrupted_provider->Shutdown();
  const auto unknown =
      interrupted_provider->ObserveOutputResult(write.identity);
  ASSERT_TRUE(unknown.ok());
  EXPECT_EQ(unknown.value().evidence,
            core::IoOutputEvidence::kAcceptanceUnknown);
  ASSERT_TRUE(unknown.value().error);
  EXPECT_EQ(unknown.value().error->code, "io_output_acceptance_unknown");
  EXPECT_EQ(interrupted_provider->SubmitOutput(write).value(), unknown.value());
  EXPECT_FALSE(interrupted_provider->SubmitOutput(request(2)).ok());
  EXPECT_EQ(send_clock.reads, 2U);
  ASSERT_TRUE(interrupted_provider->RetireOutput(write.identity).ok());
  interrupted_provider.reset(); // quiescent callback state before clock dies
}

TEST_F(RosBooleanOutputProviderTest,
       TerminalOccurrenceAtMaximumGenerationKeepsPendingAckAndOtherAlias) {
  Build();
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  ASSERT_TRUE(provider->Resume(occurrence("a"), maximum, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("b"), maximum, owner("b")).ok());
  const auto first = request(1U, true);
  const auto submitted = provider->SubmitOutput(first);
  ASSERT_TRUE(submitted.ok());
  ASSERT_EQ(submitted.value().evidence, core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_FALSE(
      provider->ShutdownOccurrence(occurrence("foreign"), owner("foreign"))
          .ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), maximum, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), maximum, owner("a")).ok());
  EXPECT_EQ(provider->SubmitOutput(first).value(), submitted.value());
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(first.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  ASSERT_EQ(received, std::vector<bool>{true});
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  auto closed = request(2U, false);
  closed.signal_id = signal("component.a", "alias");
  const auto refused = provider->SubmitOutput(closed);
  ASSERT_TRUE(refused.ok());
  EXPECT_EQ(refused.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(provider->RetireOutput(closed.identity).ok());
  const auto other = request(3U, false, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(other).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(other.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  EXPECT_EQ(received, (std::vector<bool>{true, false}));
}

TEST_F(RosBooleanOutputProviderTest,
       EqualOccurrenceReaddKeepsOldAckAndRejectsOldOwnerLifecycle) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2U, owner("b")).ok());
  const auto old = request(1U);
  ASSERT_EQ(provider->SubmitOutput(old).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  auto candidate = channel().aliases;
  auto removed = candidate;
  std::erase_if(
      removed, [](const auto &row) { return row.provider == occurrence("a"); });
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(candidate), removed,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  const auto new_owner = std::make_shared<const int>(7);
  for (auto &alias : candidate)
    if (alias.provider == occurrence("a"))
      alias.actual_driver_owner = new_owner;
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(removed), candidate,
                                  core::ActiveBundlePublicationGeneration(3U))
                  .ok());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, new_owner).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), 99U, owner("a")).ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 99U, owner("a")).ok());
  EXPECT_FALSE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_EQ(provider->SubmitOutput(old).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  EXPECT_FALSE(provider->SubmitOutput(request(2U)).ok());
  clock.set(31);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(old.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  EXPECT_EQ(provider->ObserveOutputResult(old.identity).value().identity,
            old.identity);
  EXPECT_EQ(provider->ObserveOutputResult(old.identity)
                .value()
                .trusted_received_at_ns,
            31);
  ASSERT_EQ(received, std::vector<bool>{true});
  ASSERT_TRUE(provider->RetireOutput(old.identity).ok());
  const auto other = request(3U, false, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(other).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(other.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  ASSERT_TRUE(provider->RetireOutput(other.identity).ok());
  EXPECT_EQ(provider->SubmitOutput(request(4U)).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(request(4U).identity) ==
           core::IoOutputEvidence::kSourceAccepted;
  }));
  EXPECT_EQ(received, (std::vector<bool>{true, false, true}));
}

TEST_F(RosBooleanOutputProviderTest,
       AliasAdditionPreservesGateAndReplayWhileNewOwnerStartsPaused) {
  Build();
  const auto before = provider->ReadAliasState();
  EXPECT_EQ(before.active_publication_generation,
            core::ActiveBundlePublicationGeneration(1U));
  ASSERT_EQ(before.aliases.size(), 3U);
  EXPECT_TRUE(SameIoOutputDriverOwner(
      before.aliases.front().actual_driver_owner, owner("a")));
  EXPECT_TRUE(std::is_sorted(
      before.aliases.begin(), before.aliases.end(),
      [](const auto &a, const auto &b) { return a.signal_id < b.signal_id; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  auto candidate = channel().aliases;
  const auto added_owner = std::make_shared<const int>(8);
  candidate.push_back(
      {signal("component.c"), occurrence("c"), 1U, added_owner});
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(channel().aliases), candidate,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  const auto after = provider->ReadAliasState();
  EXPECT_EQ(after.active_publication_generation,
            core::ActiveBundlePublicationGeneration(2U));
  ASSERT_EQ(after.aliases.size(), 4U);
  EXPECT_TRUE(SameIoOutputDriverOwner(after.aliases.back().actual_driver_owner,
                                      added_owner));
  EXPECT_EQ(before.active_publication_generation,
            core::ActiveBundlePublicationGeneration(1U));
  EXPECT_EQ(before.aliases.size(),
            3U); // A returned snapshot cannot be retargeted.

  ASSERT_TRUE(provider
                  ->RebindAliases(identities(channel().aliases), candidate,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  EXPECT_FALSE(provider
                   ->RebindAliases(identities(candidate), candidate,
                                   core::ActiveBundlePublicationGeneration(1U))
                   .ok());
  auto conflict = candidate;
  conflict.pop_back();
  EXPECT_FALSE(provider
                   ->RebindAliases(identities(candidate), conflict,
                                   core::ActiveBundlePublicationGeneration(2U))
                   .ok());
  EXPECT_FALSE(provider
                   ->RebindAliases({}, conflict,
                                   core::ActiveBundlePublicationGeneration(3U))
                   .ok());
  const auto added = request(1U, true, "component.c", "c");
  EXPECT_EQ(provider->SubmitOutput(added).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(provider->RetireOutput(added.identity).ok());
  const auto retained = request(2U);
  EXPECT_EQ(provider->SubmitOutput(retained).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(retained.identity) ==
           core::IoOutputEvidence::kSourceAccepted;
  }));
  EXPECT_EQ(received, std::vector<bool>{true});
}

TEST_F(RosBooleanOutputProviderTest, ExpiredActualOwnerCannotSubmitNewSend) {
  Build();
  auto configuration = channel();
  auto actual_owner = std::make_shared<const int>(9);
  for (auto &alias : configuration.aliases)
    if (alias.provider == occurrence("a"))
      alias.actual_driver_owner = actual_owner;
  auto created = RosBooleanOutputProvider::Create(node, clock, configuration);
  ASSERT_TRUE(created.ok());
  provider = std::move(created.value());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, actual_owner).ok());
  const std::weak_ptr<const void> weak = actual_owner;
  actual_owner.reset();
  ASSERT_TRUE(weak.expired());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3U, weak).ok());
  EXPECT_EQ(provider->SubmitOutput(request(1U)).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  EXPECT_TRUE(received.empty());
  EXPECT_TRUE(provider->ShutdownOccurrence(occurrence("a"), weak).ok());
}

TEST_F(RosBooleanOutputProviderTest,
       SetupRefusesUnresolvedRemappedAndAmbiguousChannels) {
  for (const auto *name :
       {"relative", "~/private", "/", "/io//set", "/io/{name}"}) {
    auto config = channel();
    config.address = RosSetBoolOutputAddress{name};
    EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, config).ok());
  }
  auto config = channel();
  config.aliases.push_back(config.aliases.front());
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, config).ok());
  config = channel();
  config.aliases[1].prepared_driver_generation = 2;
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, config).ok());
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  options.arguments(
      {"--ros-args", "-r",
       std::get<RosSetBoolOutputAddress>(channel().address).service +
           ":=/different_service"});
  auto remapped =
      std::make_shared<rclcpp::Node>("remapped_output_test", options);
  EXPECT_FALSE(
      RosBooleanOutputProvider::Create(remapped, clock, channel()).ok());
  auto stopped_context = std::make_shared<rclcpp::Context>();
  stopped_context->init(0, nullptr);
  options = rclcpp::NodeOptions();
  options.context(stopped_context);
  auto stopped = std::make_shared<rclcpp::Node>("stopped_output_test", options);
  stopped_context->shutdown("exercise output creation failure");
  EXPECT_FALSE(
      RosBooleanOutputProvider::Create(stopped, clock, channel()).ok());
}

} // namespace
TEST_F(RosBooleanOutputProviderTest,
       UrServiceMapsExactDigitalRequestAndCorrelatedAcceptance) {
  using UrService = ur_msgs::srv::SetIO;
  std::vector<UrService::Request> requests;
  bool source_accepts = true;
  auto ur_service = server_node->create_service<UrService>(
      "/test/set_io", [&](UrService::Request::SharedPtr request,
                          UrService::Response::SharedPtr response) {
        requests.push_back(*request);
        response->success = source_accepts;
      });
  auto configuration = channel();
  configuration.address = UrSetIoOutputAddress{"/test/set_io", 17U};
  auto created = RosBooleanOutputProvider::Create(node, clock, configuration);
  ASSERT_TRUE(created.ok());
  provider = std::move(created.value());
  ASSERT_TRUE(SpinUntil([&] { return provider->ServiceReady(); }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  const auto first = request(1U);
  EXPECT_EQ(provider->SubmitOutput(first).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(first.identity) == core::IoOutputEvidence::kSourceAccepted;
  }));
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests.front().fun, UrService::Request::FUN_SET_DIGITAL_OUT);
  EXPECT_EQ(requests.front().pin, 17);
  EXPECT_EQ(requests.front().state, 1.0F);
  EXPECT_EQ(provider->StrongestOutputEvidence(),
            core::IoOutputEvidence::kSourceAccepted);
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  source_accepts = false;
  const auto second = request(2U, false);
  ASSERT_TRUE(provider->SubmitOutput(second).ok());
  ASSERT_TRUE(SpinUntil([&] {
    return Evidence(second.identity) == core::IoOutputEvidence::kSourceRejected;
  }));
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests.back().state, 0.0F);
  auto invalid = channel();
  invalid.address = UrSetIoOutputAddress{"/test/set_io", 18U};
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, invalid).ok());
}

TEST_F(RosBooleanOutputProviderTest,
       GpioPublishesOnlyExactSparseChannelAndNeverClaimsAcknowledgement) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscription = server_node->create_subscription<Message>(
      "/test/gpio_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &value) { messages.push_back(value); });
  auto configuration = channel();
  configuration.address =
      RosGpioOutputAddress{"/test/gpio_commands", "bank/a", "digital.out"};
  auto created = RosBooleanOutputProvider::Create(node, clock, configuration);
  ASSERT_TRUE(created.ok());
  provider = std::move(created.value());
  ASSERT_TRUE(
      SpinUntil([&] { return subscription->get_publisher_count() == 1U; }));
  EXPECT_EQ(provider->StrongestOutputEvidence(),
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  auto first = request(1U, false);
  first.absolute_deadline_ns.reset();
  const auto submitted = provider->SubmitOutput(first);
  ASSERT_TRUE(submitted.ok());
  EXPECT_EQ(submitted.value().evidence, core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  EXPECT_EQ(messages.front().interface_groups,
            std::vector<std::string>{"bank/a"});
  ASSERT_EQ(messages.front().interface_values.size(), 1U);
  EXPECT_EQ(messages.front().interface_values.front().interface_names,
            std::vector<std::string>{"digital.out"});
  EXPECT_EQ(messages.front().interface_values.front().values,
            std::vector<double>{0.0});
  EXPECT_EQ(provider->SubmitOutput(first).value(), submitted.value());
  provider->Shutdown();
  EXPECT_EQ(Evidence(first.identity), core::IoOutputEvidence::kSubmitted);
  EXPECT_EQ(messages.size(), 1U); // no automatic resend or fabricated ACK
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
}

TEST_F(RosBooleanOutputProviderTest,
       UrFinalEffectFenceRefusesElapsedBudgetWithoutSending) {
  class SendFenceClock final : public core::IClock {
  public:
    core::TimePoint now() const override {
      if (++reads == 2U)
        clock.set(10);
      return clock.now();
    }
    mutable core::FakeClock clock;
    mutable unsigned reads{};
  } send_clock;
  using UrService = ur_msgs::srv::SetIO;
  unsigned sends = 0;
  auto ur_service = server_node->create_service<UrService>(
      "/test/set_io_deadline", [&](UrService::Request::SharedPtr,
                                   UrService::Response::SharedPtr response) {
        ++sends;
        response->success = true;
      });
  auto configuration = channel();
  configuration.address = UrSetIoOutputAddress{"/test/set_io_deadline", 1U};
  auto created =
      RosBooleanOutputProvider::Create(node, send_clock, configuration);
  ASSERT_TRUE(created.ok());
  auto candidate = std::move(created.value());
  ASSERT_TRUE(SpinUntil([&] { return candidate->ServiceReady(); }));
  ASSERT_TRUE(candidate->Resume(occurrence("a"), 2U, owner("a")).ok());
  auto write = request(1U);
  write.absolute_deadline_ns = 10;
  const auto refused = candidate->SubmitOutput(write);
  ASSERT_TRUE(refused.ok());
  EXPECT_EQ(refused.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(refused.value().error->details.at("reason"),
            "io_output_deadline_expired");
  EXPECT_EQ(send_clock.reads, 2U);
  server_executor->spin_some();
  client_executor->spin_some();
  EXPECT_EQ(sends, 0U);
  EXPECT_EQ(candidate->SubmitOutput(write).value(), refused.value());
  candidate->Shutdown();
}

TEST_F(RosBooleanOutputProviderTest,
       GpioWholeTopicCarriesKnownChannelsAcrossGroupsOnlyOnExplicitWrite) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto made_a = RosBooleanOutputProvider::Create(
      node, clock, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(made_a.ok());
  provider = std::move(made_a.value());
  auto made_b = RosBooleanOutputProvider::Create(
      node, clock, gpio_channel("b", "group.b", "two"), provider->gpio_topic());
  ASSERT_TRUE(made_b.ok());
  auto other = std::move(made_b.value());
  ASSERT_TRUE(
      SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  auto first = request(1U, false);
  first.absolute_deadline_ns.reset();
  EXPECT_EQ(provider->SubmitOutput(first).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  EXPECT_EQ(gpio_values(messages.back()),
            (GpioValues{{"group.a", {{"one", 0.0}}}}));
  auto second = request(2U, true, "component.b", "b");
  second.absolute_deadline_ns.reset();
  EXPECT_EQ(other->SubmitOutput(second).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
  EXPECT_EQ(
      gpio_values(messages.back()),
      (GpioValues{{"group.a", {{"one", 0.0}}}, {"group.b", {{"two", 1.0}}}}));
  EXPECT_EQ(other->SubmitOutput(second).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  EXPECT_EQ(messages.size(), 2U); // replay publishes nothing
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  // A provable pre-send refusal cannot erase a prior known submitted value.
  auto refused = request(4U, true); // GPIO does not accept a service deadline.
  ASSERT_EQ(provider->SubmitOutput(refused).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(provider->RetireOutput(refused.identity).ok());
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  auto expected = provider->ReadAliasState();
  auto retained = gpio_channel("a", "group.a", "one");
  retained.aliases.front().prepared_driver_generation = 4U;
  ASSERT_TRUE(provider
                  ->RebindAliases(expected.aliases, retained.aliases,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  EXPECT_EQ(messages.size(), 2U); // resume/rebind are not resend policies
  const auto other_expected = other->ReadAliasState();
  ASSERT_TRUE(other
                  ->RebindAliases(other_expected.aliases,
                                  gpio_channel("b", "group.b", "two").aliases,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  ASSERT_TRUE(other->RetireOutput(second.identity).ok());
  second = request(3U, false, "component.b", "b");
  second.absolute_deadline_ns.reset();
  ASSERT_EQ(other->SubmitOutput(second).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 3U; }));
  EXPECT_EQ(
      gpio_values(messages.back()),
      (GpioValues{{"group.a", {{"one", 0.0}}}, {"group.b", {{"two", 0.0}}}}));
}

TEST_F(
    RosBooleanOutputProviderTest,
    GpioPauseRemovalNewOwnerAndExclusionSerializeAgainstOtherChannelPublication) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  for (int action = 0; action < 4; ++action) {
    class HeldSendClock final : public core::IClock {
    public:
      core::TimePoint now() const override {
        if (++reads == 2U) {
          entered.set_value();
          release.wait();
        }
        return 0;
      }
      mutable std::atomic<unsigned> reads{};
      mutable std::promise<void> entered;
      std::shared_future<void> release;
    } held;
    std::promise<void> release;
    held.release = release.get_future().share();
    auto entered = held.entered.get_future();
    const auto topic = "/test/group_race_" + std::to_string(action);
    std::vector<Message> messages;
    auto subscriber = server_node->create_subscription<Message>(
        topic, rclcpp::SystemDefaultsQoS(),
        [&](const Message &value) { messages.push_back(value); });
    auto configuration = gpio_channel("a", "group.a", "one", topic);
    auto made_a = RosBooleanOutputProvider::Create(node, clock, configuration);
    ASSERT_TRUE(made_a.ok());
    auto a = std::move(made_a.value());
    auto made_b = RosBooleanOutputProvider::Create(
        node, held, gpio_channel("b", "group.b", "two", topic),
        a->gpio_topic());
    ASSERT_TRUE(made_b.ok());
    auto b = std::move(made_b.value());
    ASSERT_TRUE(
        SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
    ASSERT_TRUE(a->Resume(occurrence("a"), 2U, owner("a")).ok());
    ASSERT_TRUE(b->Resume(occurrence("b"), 2U, owner("b")).ok());
    auto first = request(10U, true);
    first.absolute_deadline_ns.reset();
    ASSERT_EQ(a->SubmitOutput(first).value().evidence,
              core::IoOutputEvidence::kSubmitted);
    ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
    const auto expected = a->ReadAliasState();
    const auto expected_b = b->ReadAliasState();
    auto next = request(11U, true, "component.b", "b");
    next.absolute_deadline_ns.reset();
    auto published =
        std::async(std::launch::async, [&] { return b->SubmitOutput(next); });
    const auto selected = entered.wait_for(2s);
    EXPECT_EQ(selected, std::future_status::ready);
    auto replacement_owner = std::make_shared<const int>(42);
    auto aliases = configuration.aliases;
    aliases.front().actual_driver_owner = replacement_owner;
    std::promise<void> started;
    auto started_future = started.get_future();
    auto changed = std::async(std::launch::async, [&] {
      started.set_value();
      if (action == 0)
        return a->Pause(occurrence("a"), 3U, owner("a"));
      if (action == 3)
        return a->ExcludeFromGroupModel(first.identity);
      if (action == 1)
        return b->RebindAliases(
            expected_b.aliases,
            gpio_channel("b", "group.b", "two", topic).aliases,
            core::ActiveBundlePublicationGeneration(2U));
      return a->RebindAliases(expected.aliases, aliases,
                              core::ActiveBundlePublicationGeneration(2U));
    });
    EXPECT_EQ(started_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(changed.wait_for(50ms), std::future_status::timeout);
    release.set_value(); // always release before fatal assertions/future
                         // destruction
    EXPECT_TRUE(published.get().ok());
    EXPECT_TRUE(changed.get().ok());
    ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
    EXPECT_EQ(gpio_values(messages.back()).at("group.a").at("one"), 1.0);
    if (action == 2) {
      ASSERT_TRUE(a->Resume(occurrence("a"), 2U, replacement_owner).ok());
      EXPECT_FALSE(a->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
      ASSERT_TRUE(
          b->RebindAliases(expected_b.aliases,
                           gpio_channel("b", "group.b", "two", topic).aliases,
                           core::ActiveBundlePublicationGeneration(2U))
              .ok());
    }
    ASSERT_TRUE(b->RetireOutput(next.identity).ok());
    next = request(12U, false, "component.b", "b");
    next.absolute_deadline_ns.reset();
    ASSERT_EQ(b->SubmitOutput(next).value().evidence,
              core::IoOutputEvidence::kSubmitted);
    ASSERT_TRUE(SpinUntil([&] { return messages.size() == 3U; }));
    EXPECT_FALSE(gpio_values(messages.back()).contains("group.a"));
    EXPECT_EQ(a->ObserveOutputResult(first.identity).value().evidence,
              core::IoOutputEvidence::kSubmitted);
    if (action == 1) {
      EXPECT_EQ(a->ReadAliasState().active_publication_generation,
                core::ActiveBundlePublicationGeneration(1U));
      EXPECT_EQ(identities(configuration.aliases).size(),
                a->ReadAliasState().aliases.size());
      EXPECT_EQ(b->ReadAliasState().active_publication_generation,
                core::ActiveBundlePublicationGeneration(2U));
    }
    a->Shutdown();
    b->Shutdown();
  }
}

TEST_F(RosBooleanOutputProviderTest,
       ActualEngineLostSubmitReturnExcludesKnownLeafValueBeforeUnknownEscapes) {
  class LostReturn final : public core::IIoOutputProvider {
  public:
    explicit LostReturn(RosBooleanOutputProvider &leaf) : leaf(leaf) {}
    core::IoOutputEvidence StrongestOutputEvidence() const noexcept override {
      return leaf.StrongestOutputEvidence();
    }
    core::Result<core::IoOutputObservationDTO>
    SubmitOutput(const core::IoOutputWriteRequestDTO &request) override {
      retained = request.identity;
      auto result = leaf.SubmitOutput(request);
      if (result.ok() &&
          result.value().evidence == core::IoOutputEvidence::kSubmitted)
        throw std::runtime_error("lost actual publication return");
      return result;
    }
    core::Result<core::IoOutputObservationDTO> ObserveOutputResult(
        const core::IoOutputCommandIdentityDTO &identity) const override {
      return leaf.ObserveOutputResult(identity);
    }
    core::Result<void> ExcludeFromGroupModel(
        const core::IoOutputCommandIdentityDTO &identity) override {
      return leaf.ExcludeFromGroupModel(identity);
    }
    core::Result<void>
    RetireOutput(const core::IoOutputCommandIdentityDTO &identity) override {
      return leaf.RetireOutput(identity);
    }
    RosBooleanOutputProvider &leaf;
    std::optional<core::IoOutputCommandIdentityDTO> retained;
  };
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto configuration = gpio_channel("a", "group.a", "one");
  auto made = RosBooleanOutputProvider::Create(node, clock, configuration);
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  auto made_b = RosBooleanOutputProvider::Create(
      node, clock, gpio_channel("b", "group.b", "two"), provider->gpio_topic());
  ASSERT_TRUE(made_b.ok());
  auto other = std::move(made_b.value());
  ASSERT_TRUE(
      SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  LostReturn transport(*provider);
  core::IoOutputParticipantEngine engine(transport, clock);
  auto command = submitted_engine_request(configuration.aliases.front(),
                                          configuration.resource_id);
  const auto digest = command.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(command));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(
                      prepared.value(), digest,
                      core::ControllerGoalId("gpio.goal"),
                      core::ControllerGoalGeneration(1U))
                  .ok());
  ASSERT_TRUE(transport.retained);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  EXPECT_EQ(provider->ObserveOutputResult(*transport.retained).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  const auto observed =
      engine.ObservePreparedCapabilityCommand(prepared.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().output_observation);
  EXPECT_EQ(observed.value().output_observation->evidence,
            core::IoOutputEvidence::kAcceptanceUnknown);
  ASSERT_TRUE(engine.get_status().fact);
  EXPECT_EQ(engine.get_status().fact->effect,
            core::ParticipantEffectKnowledge::kEffectUncertain);
  EXPECT_FALSE(engine.acknowledge_completed(prepared.value()).accepted);
  auto second = request(60U, false, "component.b", "b");
  second.absolute_deadline_ns.reset();
  ASSERT_TRUE(other->SubmitOutput(second).ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
  EXPECT_FALSE(gpio_values(messages.back()).contains("group.a"));
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(
                      prepared.value(), digest,
                      core::ControllerGoalId("gpio.goal"),
                      core::ControllerGoalGeneration(1U))
                  .ok());
  EXPECT_EQ(messages.size(), 2U); // exact replay never retries the publication
}

TEST_F(RosBooleanOutputProviderTest,
       GpioExactUncertaintyExclusionPreservesEvidenceAndRecoversUnwritten) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto made = RosBooleanOutputProvider::Create(
      node, clock, gpio_channel("a", "group.a", "one"));
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  auto made_b = RosBooleanOutputProvider::Create(
      node, clock, gpio_channel("b", "group.b", "two"), provider->gpio_topic());
  ASSERT_TRUE(made_b.ok());
  auto other = std::move(made_b.value());
  ASSERT_TRUE(
      SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(other->Resume(occurrence("b"), 2U, owner("b")).ok());
  auto first = request(40U);
  first.absolute_deadline_ns.reset();
  ASSERT_EQ(provider->SubmitOutput(first).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  EXPECT_FALSE(provider->ExcludeFromGroupModel(request(41U).identity).ok());
  ASSERT_TRUE(provider->ExcludeFromGroupModel(first.identity).ok());
  EXPECT_EQ(provider->ObserveOutputResult(first.identity).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  auto second = request(42U, false, "component.b", "b");
  second.absolute_deadline_ns.reset();
  ASSERT_EQ(other->SubmitOutput(second).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
  EXPECT_FALSE(gpio_values(messages.back()).contains("group.a"));
  // The actual session alone authorizes Retire after its completed66 proof.
  // Retirement starts unwritten; Enable itself is not a resend.
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  ASSERT_TRUE(other->RetireOutput(second.identity).ok());
  second = request(43U, true, "component.b", "b");
  second.absolute_deadline_ns.reset();
  ASSERT_TRUE(other->SubmitOutput(second).ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 3U; }));
  EXPECT_FALSE(gpio_values(messages.back()).contains("group.a"));
  auto current = request(44U, false);
  current.absolute_deadline_ns.reset();
  ASSERT_EQ(provider->SubmitOutput(current).value().evidence,
            core::IoOutputEvidence::kSubmitted);
  EXPECT_FALSE(provider->ExcludeFromGroupModel(first.identity).ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 4U; }));
  EXPECT_EQ(gpio_values(messages.back()).at("group.a").at("one"), 0.0);
}

TEST_F(
    RosBooleanOutputProviderTest,
    PublicationFilterExcludesLiveRemovedChannelAndPreservesExactOldEvidence) {
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  std::vector<Message> messages;
  auto subscriber = server_node->create_subscription<Message>(
      "/test/group_commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message &message) { messages.push_back(message); });
  auto configuration = gpio_channel("a", "group.a", "one");
  auto made = RosBooleanOutputProvider::Create(node, clock, configuration);
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  auto made_b = RosBooleanOutputProvider::Create(
      node, clock, gpio_channel("b", "group.b", "two"), provider->gpio_topic());
  ASSERT_TRUE(made_b.ok());
  auto b = std::move(made_b.value());
  ASSERT_TRUE(
      SpinUntil([&] { return subscriber->get_publisher_count() == 1U; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(b->Resume(occurrence("b"), 2U, owner("b")).ok());
  auto old = request(50U);
  old.absolute_deadline_ns.reset();
  const auto old_receipt = provider->SubmitOutput(old);
  ASSERT_TRUE(old_receipt.ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 1U; }));
  const auto old_metadata = provider->ReadAliasState();
  const auto current_metadata = b->ReadAliasState();
  ASSERT_TRUE(b->RebindAliases(current_metadata.aliases,
                               gpio_channel("b", "group.b", "two").aliases,
                               core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  // A is absent from the candidate publication. No empty-table commit or
  // terminal action is needed; fallible Apply cannot damage its old evidence.
  EXPECT_FALSE(provider
                   ->RebindAliases(old_metadata.aliases, {},
                                   core::ActiveBundlePublicationGeneration(2U))
                   .ok());
  EXPECT_EQ(provider->ReadAliasState().active_publication_generation,
            old_metadata.active_publication_generation);
  ASSERT_EQ(provider->ReadAliasState().aliases.size(),
            old_metadata.aliases.size());
  EXPECT_EQ(provider->ReadAliasState().aliases.front().signal_id,
            old_metadata.aliases.front().signal_id);
  EXPECT_EQ(provider->ReadAliasState().aliases.front().provider,
            old_metadata.aliases.front().provider);
  EXPECT_TRUE(SameIoOutputDriverOwner(
      provider->ReadAliasState().aliases.front().actual_driver_owner,
      old_metadata.aliases.front().actual_driver_owner));
  auto foreign_owner = std::make_shared<const int>(98);
  auto foreign_aliases = gpio_channel("b", "group.b", "two").aliases;
  foreign_aliases.front().actual_driver_owner = foreign_owner;
  EXPECT_FALSE(b->RebindAliases(current_metadata.aliases, foreign_aliases,
                                core::ActiveBundlePublicationGeneration(2U))
                   .ok());
  EXPECT_EQ(provider->ObserveOutputResult(old.identity).value(),
            old_receipt.value());
  auto second = request(51U, true, "component.b", "b");
  second.absolute_deadline_ns.reset();
  ASSERT_TRUE(b->SubmitOutput(second).ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 2U; }));
  EXPECT_FALSE(gpio_values(messages.back()).contains("group.a"));
  auto new_owner = std::make_shared<const int>(99);
  configuration.aliases.front().actual_driver_owner = new_owner;
  configuration.active_publication_generation =
      core::ActiveBundlePublicationGeneration(2U);
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, configuration,
                                                provider->gpio_topic())
                   .ok());
  // Production must first pass its existing retired-engine release gate.
  // The leaf never overwrites another live actual channel on a newer label.
  provider->Shutdown();
  auto replacement = RosBooleanOutputProvider::Create(
      node, clock, configuration, provider->gpio_topic());
  ASSERT_TRUE(replacement.ok());
  auto a = std::move(replacement.value());
  ASSERT_TRUE(a->Resume(occurrence("a"), 2U, new_owner).ok());
  ASSERT_TRUE(b->RetireOutput(second.identity).ok());
  second = request(52U, false, "component.b", "b");
  second.absolute_deadline_ns.reset();
  ASSERT_TRUE(b->SubmitOutput(second).ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 3U; }));
  EXPECT_FALSE(
      gpio_values(messages.back()).contains("group.a")); // new owner unwritten
  auto current = request(53U, false);
  current.absolute_deadline_ns.reset();
  ASSERT_TRUE(a->SubmitOutput(current).ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 4U; }));
  ASSERT_TRUE(provider->ExcludeFromGroupModel(old.identity).ok());
  ASSERT_TRUE(provider->RetireOutput(old.identity).ok());
  provider->Shutdown(); // old State cannot erase the replacement's model
  ASSERT_TRUE(b->RetireOutput(second.identity).ok());
  second = request(54U, true, "component.b", "b");
  second.absolute_deadline_ns.reset();
  ASSERT_TRUE(b->SubmitOutput(second).ok());
  ASSERT_TRUE(SpinUntil([&] { return messages.size() == 5U; }));
  EXPECT_EQ(gpio_values(messages.back()).at("group.a").at("one"), 0.0);
}

TEST_F(RosBooleanOutputProviderTest,
       IneligibleFinalOwnerDropsOnlyAfterWholeTopicPublicationUnlocks) {
  class HeldClock final : public core::IClock {
  public:
    core::TimePoint now() const override {
      if (++reads == 2U) {
        entered.set_value();
        release.wait();
      }
      return 0;
    }
    mutable std::atomic<unsigned> reads{};
    mutable std::promise<void> entered;
    std::shared_future<void> release;
  } held;
  // This real private control block invokes the same exact leaf terminal
  // action as DriverRecord destruction. It grants no CCLC/Kernel authority.
  struct DriverLifetime {
    std::function<void()> on_destroy;
    ~DriverLifetime() {
      if (on_destroy)
        on_destroy();
    }
  };
  auto lifetime = std::make_shared<DriverLifetime>();
  auto configuration = gpio_channel("a", "group.a", "one");
  configuration.aliases.front().actual_driver_owner = lifetime;
  auto made = RosBooleanOutputProvider::Create(node, clock, configuration);
  ASSERT_TRUE(made.ok());
  std::shared_ptr<RosBooleanOutputProvider> a(std::move(made.value()));
  std::weak_ptr<RosBooleanOutputProvider> weak_a = a;
  const std::weak_ptr<const void> exact_owner = lifetime;
  auto destroyed = std::make_shared<std::promise<bool>>();
  auto destruction = destroyed->get_future();
  lifetime->on_destroy = [weak_a, exact_owner, destroyed] {
    auto actual = weak_a.lock();
    destroyed->set_value(
        actual &&
        actual->ShutdownOccurrence(occurrence("a"), exact_owner).ok());
  };
  auto made_b = RosBooleanOutputProvider::Create(
      node, held, gpio_channel("b", "group.b", "two"), a->gpio_topic());
  ASSERT_TRUE(made_b.ok());
  auto b = std::move(made_b.value());
  ASSERT_TRUE(a->Resume(occurrence("a"), 2U, lifetime).ok());
  ASSERT_TRUE(b->Resume(occurrence("b"), 2U, owner("b")).ok());
  auto first = request(70U);
  first.absolute_deadline_ns.reset();
  ASSERT_TRUE(a->SubmitOutput(first).ok());
  ASSERT_TRUE(a->Pause(occurrence("a"), 3U, lifetime).ok());
  std::promise<void> release;
  held.release = release.get_future().share();
  auto entered = held.entered.get_future();
  auto next = request(71U, true, "component.b", "b");
  next.absolute_deadline_ns.reset();
  auto published =
      std::async(std::launch::async, [&] { return b->SubmitOutput(next); });
  EXPECT_EQ(entered.wait_for(2s), std::future_status::ready);
  // B holds the shared topic mutex after examining the disabled A member.
  // The caller can release its last pin immediately; B must retain that pin
  // until after unlocking, so the destructor cannot re-enter either mutex.
  auto dropped =
      std::async(std::launch::async,
                 [pin = std::move(lifetime)]() mutable { pin.reset(); });
  EXPECT_EQ(dropped.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(destruction.wait_for(50ms), std::future_status::timeout);
  release.set_value(); // always release before fatal checks/future destruction
  dropped.get();
  EXPECT_TRUE(published.get().ok());
  ASSERT_EQ(destruction.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(destruction.get());
  EXPECT_EQ(a->ObserveOutputResult(first.identity).value().evidence,
            core::IoOutputEvidence::kSubmitted);
}

TEST_F(RosBooleanOutputProviderTest,
       GpioTopicPinRequiresActualContextAddressAndUniqueCanonicalChannel) {
  auto configuration = gpio_channel("a", "group.a", "one");
  auto made = RosBooleanOutputProvider::Create(node, clock, configuration);
  ASSERT_TRUE(made.ok());
  provider = std::move(made.value());
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, configuration,
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
                   foreign_node, clock, gpio_channel("b", "group.b", "two"),
                   provider->gpio_topic())
                   .ok());
  foreign_node.reset();
  foreign_context->shutdown("GPIO context mismatch checked");
  auto duplicate = gpio_channel("b", "group.a", "one");
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, duplicate,
                                                provider->gpio_topic())
                   .ok());
  auto wrong_topic = gpio_channel("b", "group.b", "two", "/test/another_group");
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, wrong_topic,
                                                provider->gpio_topic())
                   .ok());
  EXPECT_FALSE(RosBooleanOutputProvider::Create(node, clock, channel(),
                                                provider->gpio_topic())
                   .ok());
}

} // namespace manyforge::roboplan_backend
