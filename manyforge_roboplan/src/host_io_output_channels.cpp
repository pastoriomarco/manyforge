#include "host_io_output_channels.hpp"

#include <algorithm>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {
namespace {
using Entries = std::vector<HostIoOutputChannelEntry>;
using Reply = core::Result<Entries>;

Reply invalid(std::string reason) {
  return Reply::Err({"invalid_request",
                     "Host output channel installation is invalid",
                     {{"reason", std::move(reason)}}});
}

struct Channel final {
  std::string profile;
  std::vector<std::string> address;
  std::optional<RosBooleanOutputAddress> ros_address;
  std::vector<HostIoOutputAliasInstallation> aliases;
  core::ActiveBundlePublicationGeneration publication{0U};
};

struct Occurrence final {
  core::ResolvedLifecycleProviderOccurrenceDTO provider;
  std::uint64_t generation;
  std::weak_ptr<const void> owner;
  core::ActiveBundlePublicationGeneration publication;
};
} // namespace

Reply CreateHostIoOutputChannels(
    std::vector<HostIoOutputAliasInstallation> aliases,
    rclcpp::Node::SharedPtr node, core::IClock &clock,
    std::span<const HostIoGpioTopicPin> captured_topics) {
  try {
    // Ephemeral selection of actual provider pins, not a second topic model.
    // Reject conflicting captures before constructing any leaf or engine.
    std::map<std::string, std::shared_ptr<RosBooleanOutputProvider::GpioTopic>>
        gpio_topics;
    for (const auto &captured : captured_topics) {
      if (captured.topic.empty() || captured.topic.front() != '/' ||
          captured.topic.find('\0') != std::string::npos || !captured.pin)
        return invalid("host_io_output_gpio_topic_unqualified");
      const auto [known, first] =
          gpio_topics.emplace(captured.topic, captured.pin);
      if (!first && (known->second.get() != captured.pin.get() ||
                     known->second.owner_before(captured.pin) ||
                     captured.pin.owner_before(known->second)))
        return invalid("host_io_output_gpio_topic_conflict");
    }
    std::map<core::ResourceId, Channel> channels;
    std::map<std::pair<std::string, std::vector<std::string>>, core::ResourceId>
        addresses;
    std::set<core::IoSignalId> signals;
    std::map<core::ControlDomainId, Occurrence> occurrences;
    // Validate the complete captured installation before creating any client.
    for (auto &alias : aliases) {
      const auto &row = alias.row;
      const auto &descriptor = row.descriptor;
      if (descriptor.direction != core::IoSignalDirection::kOutput ||
          descriptor.value_type != core::IoSignalValueType::kBoolean ||
          !descriptor.output_resource || descriptor.output_resource->empty() ||
          !core::ComputeIoSignalDescriptorDigest(descriptor).ok() ||
          !alias.provider.locally_valid() ||
          row.runtime_unit_id != alias.provider.provider_runtime_unit_id ||
          alias.prepared_driver_generation == 0U ||
          alias.actual_driver_owner.expired() ||
          alias.active_publication_generation.value() == 0U)
        return invalid("host_io_output_alias_unqualified");
      if (!signals.insert(descriptor.signal_id).second)
        return invalid("host_io_output_alias_ambiguous");
      const auto [occurrence, first_occurrence] = occurrences.emplace(
          alias.provider.control_domain_id,
          Occurrence{alias.provider, alias.prepared_driver_generation,
                     alias.actual_driver_owner,
                     alias.active_publication_generation});
      if (!first_occurrence &&
          (occurrence->second.provider != alias.provider ||
           occurrence->second.generation != alias.prepared_driver_generation ||
           !SameIoOutputDriverOwner(occurrence->second.owner,
                                    alias.actual_driver_owner) ||
           occurrence->second.publication !=
               alias.active_publication_generation))
        return invalid("host_io_output_occurrence_conflict");

      const auto &configuration = row.provider;
      const auto &profile = configuration.profile;
      std::vector<std::string> address;
      std::optional<RosBooleanOutputAddress> ros_address;
      if (configuration.input_bit || configuration.initial_value ||
          configuration.ingress_depth != 0U)
        return invalid("host_io_output_profile_unqualified");
      if (profile == "mock_output" && configuration.channel &&
          !configuration.service && !configuration.topic &&
          !configuration.pin && !configuration.group &&
          !configuration.interface_name) {
        address = {*configuration.channel};
      } else if (profile == "ros_set_bool" && configuration.service && node &&
                 !configuration.channel && !configuration.topic &&
                 !configuration.pin && !configuration.group &&
                 !configuration.interface_name) {
        address = {*configuration.service};
        ros_address = RosSetBoolOutputAddress{*configuration.service};
      } else if (profile == "ur_set_io" && configuration.service &&
                 configuration.pin && *configuration.pin <= 17U && node &&
                 !configuration.channel && !configuration.topic &&
                 !configuration.group && !configuration.interface_name) {
        // The typed leaf fixes FUN_SET_DIGITAL_OUT; no caller-selected
        // function.
        address = {*configuration.service, "set_digital_out",
                   std::to_string(*configuration.pin)};
        ros_address =
            UrSetIoOutputAddress{*configuration.service,
                                 static_cast<std::uint8_t>(*configuration.pin)};
      } else if (profile == "ros2_control_gpio" && configuration.topic &&
                 configuration.group && configuration.interface_name && node &&
                 !configuration.channel && !configuration.service &&
                 !configuration.pin) {
        address = {*configuration.topic, *configuration.group,
                   *configuration.interface_name};
        ros_address =
            RosGpioOutputAddress{*configuration.topic, *configuration.group,
                                 *configuration.interface_name};
      } else {
        return invalid("host_io_output_profile_unqualified");
      }
      if (std::any_of(address.begin(), address.end(),
                      [](const auto &part) { return part.empty(); }))
        return invalid("host_io_output_profile_unqualified");

      const auto &resource = *descriptor.output_resource;
      const auto [same_address, first_address] =
          addresses.emplace(std::pair{profile, address}, resource);
      if (!first_address && same_address->second != resource)
        return invalid("host_io_output_resource_conflict");
      const auto [channel, first_channel] = channels.emplace(
          resource, Channel{profile,
                            address,
                            std::move(ros_address),
                            {},
                            alias.active_publication_generation});
      if (!first_channel && (channel->second.profile != profile ||
                             channel->second.address != address))
        return invalid("host_io_output_address_conflict");
      if (channel->second.publication != alias.active_publication_generation)
        return invalid("host_io_output_publication_conflict");
      channel->second.aliases.push_back(std::move(alias));
    }

    Entries result;
    result.reserve(channels.size());
    for (auto &[resource, channel] : channels) {
      if (channel.profile == "mock_output") {
        std::vector<MockBooleanOutputAlias> installed;
        installed.reserve(channel.aliases.size());
        for (const auto &alias : channel.aliases)
          installed.push_back({alias.row.descriptor.signal_id, alias.provider,
                               alias.prepared_driver_generation,
                               alias.actual_driver_owner});
        auto made = MockBooleanOutputProvider::Create(
            clock, {resource, channel.address.front(), std::move(installed),
                    channel.publication});
        if (!made.ok())
          return Reply::Err(made.error());
        auto provider =
            std::shared_ptr<MockBooleanOutputProvider>(std::move(made.value()));
        auto engine =
            std::make_unique<core::IoOutputParticipantEngine>(*provider, clock);
        result.emplace_back(resource, std::move(provider), std::move(engine));
      } else {
        std::vector<RosBooleanOutputAlias> installed;
        installed.reserve(channel.aliases.size());
        for (const auto &alias : channel.aliases)
          installed.push_back({alias.row.descriptor.signal_id, alias.provider,
                               alias.prepared_driver_generation,
                               alias.actual_driver_owner});
        const auto *gpio =
            std::get_if<RosGpioOutputAddress>(&*channel.ros_address);
        std::shared_ptr<RosBooleanOutputProvider::GpioTopic> topic;
        if (gpio) {
          const auto actual = gpio_topics.find(gpio->topic);
          if (actual != gpio_topics.end())
            topic = actual->second;
        }
        auto made = RosBooleanOutputProvider::Create(
            node, clock,
            {resource, *channel.ros_address, std::move(installed),
             channel.publication},
            std::move(topic));
        if (!made.ok())
          return Reply::Err(made.error());
        auto provider =
            std::shared_ptr<RosBooleanOutputProvider>(std::move(made.value()));
        if (gpio) {
          const auto actual = provider->gpio_topic();
          if (!actual)
            return invalid("host_io_output_gpio_topic_unavailable");
          gpio_topics.emplace(gpio->topic, actual);
        }
        auto engine =
            std::make_unique<core::IoOutputParticipantEngine>(*provider, clock);
        result.emplace_back(resource, std::move(provider), std::move(engine));
      }
    }
    return Reply::Ok(std::move(result));
  } catch (const std::exception &) {
    return Reply::Err({"internal_error",
                       "Host output channels could not be created",
                       {{"reason", "host_io_output_channel_creation_failed"}}});
  }
}
} // namespace manyforge::roboplan_backend::host_detail
