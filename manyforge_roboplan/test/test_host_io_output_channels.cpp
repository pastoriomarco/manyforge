#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include "host_io_output_channels.hpp"

#include "../../manyforge_deployment/test/io_retained_fixture.hpp"
#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <manyforge_core/time/fake_clock.hpp>
#include <unistd.h>

namespace manyforge::roboplan_backend::host_detail {
namespace {
using Object = core::JsonValueDTO::Object;
using Array = core::JsonValueDTO::Array;

class HostIoOutputChannelsTest : public ::testing::Test {
protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("manyforge-output-channel-" + std::to_string(::getpid()) + "-" +
            std::to_string(next.fetch_add(1U)));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    retained = deployment::test::materialize_boolean_io_retained_fixture(
                   root, false, true)
                   .bundle;
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  core::ResolvedLifecycleProviderOccurrenceDTO
  Occurrence(bool alias = false) const {
    const auto &domain = retained->value().cell_graph.control_domains.front();
    const auto &provider = *domain.provider();
    return {alias ? core::ControlDomainId(domain.control_domain_id.value() +
                                          ".alias")
                  : domain.control_domain_id,
            domain.control_domain_revision,
            provider.provider_instance_id,
            provider.provider_manifest_id,
            provider.provider_manifest_revision,
            provider.provider_runtime_unit_id,
            core::RuntimeEpoch("test.runtime.1"),
            provider.lifecycle_plan_catalog_digest};
  }

  std::vector<HostIoOutputAliasInstallation> Single() const {
    auto catalog = deployment::ResolveRetainedIoSignalCatalog(
        retained->value().cell_graph);
    if (!catalog.ok() || catalog.value().size() != 1U)
      throw std::logic_error("genuine retained output fixture has no channel");
    return {{catalog.value().front(), Occurrence(), 1U, driver_owner,
             core::ActiveBundlePublicationGeneration(1U)}};
  }

  // Actual catalog lowering supplies channel/resource identity. The extra
  // component and captured occurrence are a configuration/helper fixture,
  // not a claim of a sealed multi-component host installation.
  std::vector<HostIoOutputAliasInstallation>
  Aliases(bool distinct_channel = false, bool ros = false,
          std::string profile = {}, bool distinct_group = false) const {
    auto graph = retained->value().cell_graph;
    auto component = graph.controllable_components.front();
    component.controllable_component_id =
        core::ControllableComponentId("device.alias");
    graph.controllable_components.push_back(std::move(component));
    auto &signals = std::get<Array>(
        graph.runtime_units.front().configuration.at("io_signals").value);
    if (ros && profile.empty())
      profile = "ros_set_bool";
    if (!profile.empty()) {
      auto &first = std::get<Object>(signals.front().value);
      Object provider{{"profile", core::JsonValueDTO{profile}}};
      if (profile == "ros2_control_gpio") {
        provider.emplace(
            "topic", core::JsonValueDTO{std::string("/io_device/commands")});
        provider.emplace("group", core::JsonValueDTO{std::string("tool")});
        provider.emplace("interface_name",
                         core::JsonValueDTO{std::string("lamp")});
      } else {
        provider.emplace("service",
                         core::JsonValueDTO{std::string("/io_device/lamp")});
      }
      first["provider"].value.emplace<Object>(std::move(provider));
    }
    auto second = std::get<Object>(signals.front().value);
    second["controllable_component_id"] =
        core::JsonValueDTO{std::string("device.alias")};
    second["source_id"] = core::JsonValueDTO{std::string("source.alias")};
    if (distinct_channel) {
      auto &provider = std::get<Object>(second.at("provider").value);
      if (profile == "ros2_control_gpio") {
        provider["interface_name"] = core::JsonValueDTO{std::string("other")};
        if (distinct_group)
          provider["group"] = core::JsonValueDTO{std::string("other.group")};
      } else
        provider[profile == "ros_set_bool" ? "service" : "channel"] =
            core::JsonValueDTO{std::string(
                profile == "ros_set_bool" ? "/io_device/other" : "other")};
    }
    signals.emplace_back();
    signals.back().value.emplace<Object>(std::move(second));
    auto catalog = deployment::ResolveRetainedIoSignalCatalog(graph);
    if (!catalog.ok() || catalog.value().size() != 2U)
      throw std::logic_error("catalog-derived alias configuration is invalid");
    std::vector<HostIoOutputAliasInstallation> result;
    for (auto &row : catalog.value()) {
      const bool alias =
          row.descriptor.signal_id.controllable_component_id.value() == "device.alias";
      result.push_back({std::move(row), Occurrence(alias), 1U,
                        alias ? alias_driver_owner : driver_owner,
                        core::ActiveBundlePublicationGeneration(1U), 500'000'000});
    }
    return result;
  }

  static inline std::atomic<std::uint64_t> next{};
  std::filesystem::path root;
  std::shared_ptr<const deployment::RetainedBundle> retained;
  core::FakeClock clock;
  std::shared_ptr<const void> driver_owner = std::make_shared<int>(1);
  std::shared_ptr<const void> alias_driver_owner = std::make_shared<int>(2);
};

TEST_F(HostIoOutputChannelsTest,
       GenuineRetainedOutputCreatesOnePausedModel) {
  const auto aliases = Single();
  auto channels = CreateHostIoOutputChannels(aliases, {}, clock);
  ASSERT_TRUE(channels.ok());
  ASSERT_EQ(channels.value().size(), 1U);
  auto &entry = channels.value().front();
  EXPECT_EQ(entry.resource_id, aliases.front().row.descriptor.output_resource);
  const auto provider =
      std::get<std::shared_ptr<MockBooleanOutputProvider>>(entry.provider);
  ASSERT_NE(provider, nullptr);
  EXPECT_FALSE(provider->model_value());
  const auto refused =
      provider->WriteOutput(aliases.front().row.descriptor.signal_id, true);
  EXPECT_EQ(refused.outcome, core::ActionOutcome::kFailed);
  EXPECT_FALSE(provider->model_value());
}

TEST_F(HostIoOutputChannelsTest,
       CatalogAliasesShareOneModelAndDistinctLifecycleGates) {
  const auto aliases = Aliases();
  ASSERT_EQ(aliases[0].row.descriptor.output_resource,
            aliases[1].row.descriptor.output_resource);
  auto channels = CreateHostIoOutputChannels(aliases, {}, clock);
  ASSERT_TRUE(channels.ok());
  ASSERT_EQ(channels.value().size(), 1U);
  const auto provider = std::get<std::shared_ptr<MockBooleanOutputProvider>>(
      channels.value().front().provider);
  ASSERT_TRUE(provider->Resume(aliases[0].provider, 2U,
                               aliases[0].actual_driver_owner).ok());
  const auto &first = aliases[0].row.descriptor.signal_id;
  const auto &second = aliases[1].row.descriptor.signal_id;
  ASSERT_EQ(provider->WriteOutput(first, true).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(provider->WriteOutput(second, false).outcome,
            core::ActionOutcome::kFailed);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(provider->Resume(aliases[1].provider, 2U,
                               aliases[1].actual_driver_owner).ok());
  ASSERT_TRUE(provider->Pause(aliases[0].provider, 3U,
                              aliases[0].actual_driver_owner).ok());
  EXPECT_FALSE(provider->Resume(aliases[0].provider, 2U,
                                aliases[0].actual_driver_owner).ok());
  EXPECT_EQ(provider->WriteOutput(second, false).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
}

TEST_F(HostIoOutputChannelsTest,
       DistinctCatalogChannelsHaveDistinctModels) {
  auto channels = CreateHostIoOutputChannels(Aliases(true), {}, clock);
  ASSERT_TRUE(channels.ok());
  ASSERT_EQ(channels.value().size(), 2U);
  EXPECT_NE(channels.value()[0].resource_id, channels.value()[1].resource_id);
  EXPECT_NE(std::get<std::shared_ptr<MockBooleanOutputProvider>>(
                channels.value()[0].provider)
                .get(),
            std::get<std::shared_ptr<MockBooleanOutputProvider>>(
                channels.value()[1].provider)
                .get());
}

TEST_F(HostIoOutputChannelsTest,
       RejectsBothDirectionsOfResourceAddressDisagreement) {
  auto aliases = Aliases(true);
  aliases[1].row.descriptor.output_resource =
      aliases[0].row.descriptor.output_resource;
  auto result = CreateHostIoOutputChannels(std::move(aliases), {}, clock);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "host_io_output_address_conflict");
  aliases = Aliases();
  aliases[1].row.descriptor.output_resource =
      core::ResourceId("different.resource");
  result = CreateHostIoOutputChannels(std::move(aliases), {}, clock);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "host_io_output_resource_conflict");
}

TEST_F(HostIoOutputChannelsTest,
       RejectsDuplicateSignalsAndConflictingCurrentOccurrences) {
  auto aliases = Aliases();
  aliases.push_back(aliases.front());
  EXPECT_FALSE(CreateHostIoOutputChannels(std::move(aliases), {}, clock).ok());
  aliases = Aliases();
  aliases[1].provider = aliases[0].provider;
  aliases[1].prepared_driver_generation = 2U;
  EXPECT_FALSE(CreateHostIoOutputChannels(std::move(aliases), {}, clock).ok());
  aliases = Aliases();
  aliases[1].provider = aliases[0].provider;
  aliases[1].provider.provider_runtime_epoch =
      core::RuntimeEpoch("stale.runtime");
  EXPECT_FALSE(CreateHostIoOutputChannels(std::move(aliases), {}, clock).ok());
  aliases = Single();
  aliases[0].provider.provider_runtime_unit_id =
      core::RuntimeUnitId("foreign.host");
  EXPECT_FALSE(CreateHostIoOutputChannels(std::move(aliases), {}, clock).ok());
  aliases = Single();
  aliases[0].prepared_driver_generation = 0U;
  EXPECT_FALSE(CreateHostIoOutputChannels(std::move(aliases), {}, clock).ok());
  aliases = Single();
  aliases[0].row.descriptor.direction = core::IoSignalDirection::kInput;
  EXPECT_FALSE(CreateHostIoOutputChannels(std::move(aliases), {}, clock).ok());
  aliases = Single();
  aliases[0].row.descriptor.output_resource.reset();
  EXPECT_FALSE(CreateHostIoOutputChannels(std::move(aliases), {}, clock).ok());
}

TEST_F(HostIoOutputChannelsTest,
       RequiresLiveExactDriverOwnersAndOnePublicationPerChannel) {
  auto aliases = Single();
  aliases.front().actual_driver_owner.reset();
  EXPECT_FALSE(CreateHostIoOutputChannels(aliases, {}, clock).ok());
  aliases = Single();
  {
    const auto expired = std::make_shared<int>(3);
    aliases.front().actual_driver_owner = expired;
  }
  EXPECT_FALSE(CreateHostIoOutputChannels(aliases, {}, clock).ok());
  aliases = Single();
  aliases.front().active_publication_generation =
      core::ActiveBundlePublicationGeneration(0U);
  EXPECT_FALSE(CreateHostIoOutputChannels(aliases, {}, clock).ok());

  aliases = Aliases();
  aliases[1].provider = aliases[0].provider;
  const auto conflicting_owner = CreateHostIoOutputChannels(aliases, {}, clock);
  ASSERT_FALSE(conflicting_owner.ok());
  EXPECT_EQ(conflicting_owner.error().details.at("reason"),
            "host_io_output_occurrence_conflict");

  aliases[1].actual_driver_owner = aliases[0].actual_driver_owner;
  const auto shared_owner = CreateHostIoOutputChannels(aliases, {}, clock);
  ASSERT_TRUE(shared_owner.ok());
  EXPECT_EQ(shared_owner.value().size(), 1U);

  aliases = Aliases();
  aliases[1].active_publication_generation =
      core::ActiveBundlePublicationGeneration(2U);
  const auto mixed_publications = CreateHostIoOutputChannels(aliases, {}, clock);
  ASSERT_FALSE(mixed_publications.ok());
  EXPECT_EQ(mixed_publications.error().details.at("reason"),
            "host_io_output_publication_conflict");
}

TEST_F(HostIoOutputChannelsTest,
       RosUsesSuppliedNodeAndCreatesOnePausedAliasedProvider) {
  const auto aliases = Aliases(false, true);
  EXPECT_FALSE(CreateHostIoOutputChannels(aliases, {}, clock).ok());
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto node_owner = ros2_control_execution::Ros2TransportResources::create(
      "manyforge_host_output_channel_test", "", 1U, [context] { return context; });
  auto node = node_owner->node();
  {
    auto channels = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
    ASSERT_TRUE(channels.ok());
    ASSERT_EQ(channels.value().size(), 1U);
    EXPECT_TRUE(
        std::holds_alternative<std::shared_ptr<RosBooleanOutputProvider>>(
            channels.value().front().provider));
  }
  node.reset();
  node_owner->quiesce();
  node_owner.reset();
  context->shutdown("host output channel test complete");
}

TEST_F(HostIoOutputChannelsTest,
       TypedCatalogAliasesKeepTheCompleteAddress) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto node_owner = ros2_control_execution::Ros2TransportResources::create(
      "typed_host_output_channel_test", "", 1U, [context] { return context; });
  auto node = node_owner->node();
  for (const auto profile : {"ros_set_bool", "ros2_control_gpio"}) {
    const auto aliases = Aliases(false, false, profile);
    ASSERT_EQ(aliases[0].row.descriptor.output_resource,
              aliases[1].row.descriptor.output_resource);
    EXPECT_FALSE(CreateHostIoOutputChannels(aliases, {}, clock).ok());
    {
      auto channels = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
      ASSERT_TRUE(channels.ok()) << channels.error().message;
      ASSERT_EQ(channels.value().size(), 1U);
      const auto provider = std::get<std::shared_ptr<RosBooleanOutputProvider>>(
          channels.value().front().provider);
      EXPECT_EQ(static_cast<bool>(provider->gpio_topic()),
                std::string_view(profile) == "ros2_control_gpio");
      EXPECT_EQ(provider->WriteOutput(aliases[0].row.descriptor.signal_id, true, {})
                    .outcome,
                core::ActionOutcome::kFailed);
    }
    {
      // A second service or GPIO interface is a distinct channel.
      const auto distinct = Aliases(true, false, profile);
      ASSERT_NE(distinct[0].row.descriptor.output_resource,
                distinct[1].row.descriptor.output_resource);
      auto channels = CreateHostIoOutputChannels(distinct, node, clock, {}, node_owner);
      ASSERT_TRUE(channels.ok()) << channels.error().message;
      ASSERT_EQ(channels.value().size(), 2U);
    }
  }
  node.reset();
  node_owner->quiesce();
  node_owner.reset();
  context->shutdown("typed host output channel test complete");
}

TEST_F(HostIoOutputChannelsTest,
       TypedAddressBijectionAndClosedProfileFieldsRefuseBeforeConstruction) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto node_owner = ros2_control_execution::Ros2TransportResources::create(
      "typed_host_output_conflict_test", "", 1U, [context] { return context; });
  auto node = node_owner->node();
  for (const auto profile : {"ros_set_bool", "ros2_control_gpio"}) {
    auto aliases = Aliases(true, false, profile);
    aliases[1].row.descriptor.output_resource =
        aliases[0].row.descriptor.output_resource;
    auto result = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "host_io_output_address_conflict");
    aliases = Aliases(false, false, profile);
    aliases[1].row.descriptor.output_resource =
        core::ResourceId("different.resource");
    result = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "host_io_output_resource_conflict");
    aliases = Aliases(false, false, profile);
    aliases[0].row.provider.adapter_fields["pin"] =
        core::JsonValueDTO{1.0}; // No generic profile has adapter fields.
    result = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "host_io_output_profile_unqualified");
    aliases = Aliases(false, false, profile);
    aliases[0].row.provider.channel = "extra.synthetic.channel";
    result = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "host_io_output_profile_unqualified");
  }
  node.reset();
  node_owner->quiesce();
  node_owner.reset();
  context->shutdown("typed host output conflict test complete");
}

TEST_F(HostIoOutputChannelsTest,
       GpioChannelsAcrossGroupsShareTheActualWholeTopicPin) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto node_owner = ros2_control_execution::Ros2TransportResources::create(
      "gpio_topic_channel_test", "", 1U, [context] { return context; });
  auto node = node_owner->node();
  {
    const auto aliases = Aliases(true, false, "ros2_control_gpio", true);
    ASSERT_NE(aliases[0].row.provider.group, aliases[1].row.provider.group);
    ASSERT_EQ(aliases[0].row.provider.topic, aliases[1].row.provider.topic);
    auto channels = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
    ASSERT_TRUE(channels.ok()) << channels.error().message;
    ASSERT_EQ(channels.value().size(), 2U);
    const auto first = std::get<std::shared_ptr<RosBooleanOutputProvider>>(
        channels.value()[0].provider);
    const auto second = std::get<std::shared_ptr<RosBooleanOutputProvider>>(
        channels.value()[1].provider);
    ASSERT_TRUE(first->gpio_topic());
    EXPECT_EQ(first->gpio_topic(), second->gpio_topic());
    EXPECT_NE(first.get(), second.get());
  }
  node.reset();
  node_owner->quiesce();
  node_owner.reset();
  context->shutdown("GPIO whole topic helper complete");
}

TEST_F(HostIoOutputChannelsTest, OwnedGpioTopicPinSurvivesChannelReplacement) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto transport = ros2_control_execution::Ros2TransportResources::create(
      "owned_gpio_channels_test", "", 1U, [&] { return context; });
  transport->start();
  const auto aliases = Aliases(true, false, "ros2_control_gpio", true);
  auto retained = CreateHostIoOutputChannels({aliases[0]}, transport->node(), clock, {}, transport);
  ASSERT_TRUE(retained.ok());
  auto pin = std::get<std::shared_ptr<RosBooleanOutputProvider>>(
                 retained.value().front().provider)->gpio_topic();
  std::vector<HostIoGpioTopicPin> pins{{*aliases[0].row.provider.topic, pin}};
  retained.value().clear();
  auto added = CreateHostIoOutputChannels({aliases[1]}, transport->node(), clock, pins, transport);
  ASSERT_TRUE(added.ok());
  EXPECT_EQ(std::get<std::shared_ptr<RosBooleanOutputProvider>>(
                added.value().front().provider)->gpio_topic(), pin);
  added.value().clear();
  transport->quiesce();
  pins.clear();
  pin.reset();
  transport.reset();
  context->shutdown("owned GPIO channel fixture complete");
}

TEST_F(HostIoOutputChannelsTest,
       CapturedGpioTopicPinsAreSharedExactlyAndConflictsRefuse) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto node_owner = ros2_control_execution::Ros2TransportResources::create(
      "gpio_topic_predecessor_test", "", 1U, [context] { return context; });
  auto node = node_owner->node();
  auto peer_owner = ros2_control_execution::Ros2TransportResources::create(
      "gpio_topic_same_context", "", 1U, [context] { return context; });
  auto peer = peer_owner->node();
  auto foreign_context = std::make_shared<rclcpp::Context>();
  foreign_context->init(0, nullptr);
  auto foreign_owner = ros2_control_execution::Ros2TransportResources::create(
      "gpio_topic_other_context", "", 1U, [foreign_context] { return foreign_context; });
  auto foreign = foreign_owner->node();
  {
    const auto aliases = Aliases(true, false, "ros2_control_gpio", true);
    auto retained = CreateHostIoOutputChannels({aliases[0]}, node, clock, {}, node_owner);
    ASSERT_TRUE(retained.ok()) << retained.error().message;
    const auto actual = std::get<std::shared_ptr<RosBooleanOutputProvider>>(
                            retained.value().front().provider)
                            ->gpio_topic();
    ASSERT_TRUE(actual);
    const std::vector<HostIoGpioTopicPin> pins{
        {*aliases[0].row.provider.topic, actual},
        {*aliases[0].row.provider.topic, actual}};
    // A different node in the SAME actual ROS context reuses the one pin.
    auto added = CreateHostIoOutputChannels({aliases[1]}, peer, clock, pins, peer_owner);
    ASSERT_TRUE(added.ok()) << added.error().message;
    ASSERT_EQ(added.value().size(), 1U);
    EXPECT_EQ(std::get<std::shared_ptr<RosBooleanOutputProvider>>(
                  added.value().front().provider)
                  ->gpio_topic(),
              actual);
    // The label is not authority: the real leaf rejects another context.
    const auto wrong_context =
        CreateHostIoOutputChannels({aliases[1]}, foreign, clock, pins, foreign_owner);
    ASSERT_FALSE(wrong_context.ok());
    EXPECT_EQ(wrong_context.error().code, "io_ros_output_setup_failed");
    auto unrelated = CreateHostIoOutputChannels({aliases[0]}, foreign, clock, {}, foreign_owner);
    ASSERT_TRUE(unrelated.ok()) << unrelated.error().message;
    const auto other = std::get<std::shared_ptr<RosBooleanOutputProvider>>(
                           unrelated.value().front().provider)
                           ->gpio_topic();
    ASSERT_TRUE(other);
    ASSERT_NE(other, actual);
    const std::vector<HostIoGpioTopicPin> conflict{
        {*aliases[0].row.provider.topic, actual},
        {*aliases[0].row.provider.topic, other}};
    const auto refused =
        CreateHostIoOutputChannels({aliases[1]}, node, clock, conflict, node_owner);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "host_io_output_gpio_topic_conflict");
    const std::vector<HostIoGpioTopicPin> absent{
        {*aliases[0].row.provider.topic, {}}};
    const auto no_pin = CreateHostIoOutputChannels({}, node, clock, absent, node_owner);
    ASSERT_FALSE(no_pin.ok());
    EXPECT_EQ(no_pin.error().details.at("reason"),
              "host_io_output_gpio_topic_unqualified");
  }
  peer.reset();
  peer_owner->quiesce();
  peer_owner.reset();
  node.reset();
  node_owner->quiesce();
  node_owner.reset();
  foreign.reset();
  foreign_owner->quiesce();
  foreign_owner.reset();
  foreign_context->shutdown("GPIO foreign topic helper complete");
  context->shutdown("GPIO predecessor topic helper complete");
}

// UR I/O profiles are gone (specs UR_IO_FACTS.md); a row naming one is refused.
TEST_F(HostIoOutputChannelsTest, UrSetIoProfileIsRefused) {
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto node_owner = ros2_control_execution::Ros2TransportResources::create(
      "ur_profile_refused_test", "", 1U, [context] { return context; });
  auto node = node_owner->node();
  {
    auto aliases = Single();
    auto &provider = aliases.front().row.provider;
    provider.profile = "ur_set_io";
    provider.channel.reset();
    provider.adapter_fields["service"] =
        core::JsonValueDTO{std::string("/io_and_status_controller/set_io")};
    provider.adapter_fields["pin"] = core::JsonValueDTO{1.0};
    const auto result = CreateHostIoOutputChannels(aliases, node, clock, {}, node_owner);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"),
              "host_io_output_profile_unqualified");
  }
  node.reset();
  node_owner->quiesce();
  node_owner.reset();
  context->shutdown("UR profile refusal checked");
}

TEST_F(HostIoOutputChannelsTest, EmptyOutputCatalogNeedsNoSyntheticProvider) {
  const auto result = CreateHostIoOutputChannels({}, {}, clock);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().empty());
}
} // namespace
} // namespace manyforge::roboplan_backend::host_detail
