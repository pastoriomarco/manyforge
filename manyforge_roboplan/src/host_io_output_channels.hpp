#pragma once

#include <cstdint>
#include <manyforge_core/io_output_participant_engine.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_roboplan/mock_boolean_output_provider.hpp>
#include <manyforge_roboplan/ros_boolean_output_provider.hpp>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {

// The existing host installer supplies these from the exact retained catalog,
// captured provider occurrence, actual driver owner/observation and Applied
// publication. This helper does
// not qualify a graph, mint an occurrence or grant command authority.
struct HostIoOutputAliasInstallation final {
  deployment::RetainedIoSignalConfiguration row;
  core::ResolvedLifecycleProviderOccurrenceDTO provider;
  std::uint64_t prepared_driver_generation;
  std::weak_ptr<const void> actual_driver_owner;
  core::ActiveBundlePublicationGeneration active_publication_generation;
};

// Captured only from a qualified predecessor Entry and its sealed GPIO row.
// The leaf verifies the actual pin's ROS context/topic; this label grants no
// lifecycle, channel membership or command authority.
struct HostIoGpioTopicPin final {
  std::string topic;
  std::shared_ptr<RosBooleanOutputProvider::GpioTopic> pin;
};

struct HostIoOutputChannelEntry final {
  using Provider = std::variant<std::shared_ptr<MockBooleanOutputProvider>,
                                std::shared_ptr<RosBooleanOutputProvider>>;

  HostIoOutputChannelEntry(
      core::ResourceId resource, Provider output_provider,
      std::unique_ptr<core::IoOutputParticipantEngine> output_engine)
      : resource_id(std::move(resource)), provider(std::move(output_provider)),
        engine(std::move(output_engine)) {}
  HostIoOutputChannelEntry(HostIoOutputChannelEntry &&) noexcept = default;
  HostIoOutputChannelEntry(const HostIoOutputChannelEntry &) = delete;
  HostIoOutputChannelEntry &
  operator=(const HostIoOutputChannelEntry &) = delete;
  HostIoOutputChannelEntry &operator=(HostIoOutputChannelEntry &&) = delete;

  core::ResourceId resource_id;
  // Destruction order keeps the leaf alive until its engine is destroyed.
  Provider provider;
  std::unique_ptr<core::IoOutputParticipantEngine> engine;
};

// Typed SetBool/UR/GPIO addresses retain their complete qualified selectors;
// channel grouping never drops a pin, group or interface.
// One initially paused leaf and one subordinate engine per physical channel,
// with separate gates for its exact provider occurrences. Uses the existing
// node/executor/clock; does not create a worker, timer, registry or policy.
// Known predecessor pins and newly created GPIO channels share the actual
// whole-topic provider model within this call; no shadow model is retained.
// Host owns callback quiescence and must keep clock alive through destruction.
[[nodiscard]] core::Result<std::vector<HostIoOutputChannelEntry>>
CreateHostIoOutputChannels(
    std::vector<HostIoOutputAliasInstallation> aliases,
    rclcpp::Node::SharedPtr node, core::IClock &clock,
    std::span<const HostIoGpioTopicPin> captured_topics = {});

} // namespace manyforge::roboplan_backend::host_detail
