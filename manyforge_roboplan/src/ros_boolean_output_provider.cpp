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
#include <future>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <map>
#include <mutex>
#include <rclcpp/client.hpp>
#include <rclcpp/publisher.hpp>
#include <rclcpp/qos.hpp>
#include <set>
#include <std_srvs/srv/set_bool.hpp>
#include <type_traits>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {
using Service = std_srvs::srv::SetBool;
using Client = rclcpp::Client<Service>;
using GpioMessage = control_msgs::msg::DynamicInterfaceGroupValues;
using GpioPublisher = rclcpp::Publisher<GpioMessage>;
using Wire = std::variant<std::shared_ptr<Service::Request>, GpioMessage>;

[[nodiscard]] SetOutputResult write_failed(std::string code, std::string message,
                                           const core::ActionOutcome outcome =
                                               core::ActionOutcome::kFailed) {
  return {outcome, core::ActionError{std::move(code), std::move(message), std::nullopt,
                                     std::nullopt}};
}

template <class T>
core::Result<T> reject(std::string reason, std::string message) {
  return core::Result<T>::Err({"io_output_rejected",
                               std::move(message),
                               {{"reason", std::move(reason)}}});
}

bool absolute_service(std::string_view service) {
  if (service.size() < 2U || service.size() > 4096U || service.front() != '/' ||
      service.back() == '/')
    return false;
  bool start = true;
  for (const auto c : service.substr(1U)) {
    if (c == '/') {
      if (start)
        return false;
      start = true;
      continue;
    }
    const bool alpha =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (!alpha && (start || c < '0' || c > '9'))
      return false;
    start = false;
  }
  return true;
}

} // namespace

struct RosBooleanOutputProvider::State final {
  struct Written final {
    bool value;
    core::ResolvedLifecycleProviderOccurrenceDTO provider;
    std::weak_ptr<const void> actual_driver_owner;
  };
  struct Gate final {
    core::ResolvedLifecycleProviderOccurrenceDTO provider;
    std::uint64_t generation;
    std::weak_ptr<const void> actual_driver_owner;
    bool accepting = false;
    bool terminal = false;
  };
  struct Alias final {
    core::IoSignalId signal_id;
    std::size_t gate;
  };
  struct AliasTable final {
    std::vector<Gate> gates;
    std::vector<Alias> aliases;
  };
  static core::Result<AliasTable>
  PrepareAliases(const std::vector<RosBooleanOutputAlias> &configuration) {
    if (configuration.empty())
      return reject<AliasTable>("ros_output_alias_invalid",
                                "Output alias table is empty");
    AliasTable table;
    std::set<core::IoSignalId> signals;
    for (const auto &alias : configuration) {
      if (alias.signal_id.controllable_component_id.empty() ||
          alias.signal_id.name.empty() || !alias.provider.locally_valid() ||
          !alias.prepared_driver_generation ||
          alias.actual_driver_owner.expired() ||
          !signals.insert(alias.signal_id).second)
        return reject<AliasTable>(
            "ros_output_alias_invalid",
            "Output alias is missing, ambiguous or has no live actual owner");
      const auto gate = std::find_if(
          table.gates.begin(), table.gates.end(),
          [&](const auto &row) { return row.provider == alias.provider; });
      const auto index = static_cast<std::size_t>(gate - table.gates.begin());
      if (gate == table.gates.end())
        table.gates.push_back({alias.provider, alias.prepared_driver_generation,
                               alias.actual_driver_owner, false, false});
      else if (gate->generation != alias.prepared_driver_generation ||
               !SameIoOutputDriverOwner(gate->actual_driver_owner,
                                        alias.actual_driver_owner))
        return reject<AliasTable>(
            "ros_output_generation_mismatch",
            "Aliases disagree on their actual driver lifetime or observation");
      table.aliases.push_back({alias.signal_id, index});
    }
    std::sort(
        table.aliases.begin(), table.aliases.end(),
        [](const auto &a, const auto &b) { return a.signal_id < b.signal_id; });
    return core::Result<AliasTable>::Ok(std::move(table));
  }
  bool Matches(const std::vector<IoOutputAliasIdentity> &expected) const {
    if (aliases.size() != expected.size())
      return false;
    for (std::size_t i = 0; i < aliases.size(); ++i) {
      const auto &gate = gates[aliases[i].gate];
      if (aliases[i].signal_id != expected[i].signal_id ||
          gate.provider != expected[i].provider ||
          !SameIoOutputDriverOwner(gate.actual_driver_owner,
                                   expected[i].actual_driver_owner))
        return false;
    }
    return true;
  }
  bool Matches(const AliasTable &candidate) const {
    if (aliases.size() != candidate.aliases.size())
      return false;
    for (std::size_t i = 0; i < aliases.size(); ++i) {
      const auto &gate = gates[aliases[i].gate];
      const auto &other = candidate.gates[candidate.aliases[i].gate];
      if (aliases[i].signal_id != candidate.aliases[i].signal_id ||
          gate.provider != other.provider ||
          !SameIoOutputDriverOwner(gate.actual_driver_owner,
                                   other.actual_driver_owner))
        return false;
    }
    return true;
  }
  State(core::ActiveBundlePublicationGeneration publication,
        RosBooleanOutputAddress address, core::ResourceId resource)
      : publication(publication), address(std::move(address)),
        resource(std::move(resource)) {}
  bool Gpio() const {
    return std::holds_alternative<RosGpioOutputAddress>(address);
  }
  bool TopicReady() const;
  std::unique_lock<std::mutex> LockTopic() const;
  /// The wire of one write of `value` through `gate`: the service request,
  /// or the GPIO group message with every other channel's written value
  /// (`committed_model` is this channel's new one). The caller holds the
  /// channel and topic locks; the pins keep every lifetime read here alive
  /// past both. Throws when the wire can't be prepared (nothing was sent).
  static Wire BuildWire(const std::shared_ptr<State> &self, bool value, const Gate &gate,
                        std::vector<std::shared_ptr<State>> &channel_pins,
                        std::vector<std::shared_ptr<const void>> &writer_pins,
                        std::optional<Written> &committed_model);
  bool Ready() const {
    if (Gpio())
      return TopicReady();
    return std::visit(
        [](const auto &channel) {
          using Channel = std::decay_t<decltype(channel)>;
          if constexpr (std::is_same_v<Channel, GpioPublisher::SharedPtr>)
            return bool(channel);
          else
            return channel && channel->service_is_ready();
        },
        transport);
  }
  core::ActiveBundlePublicationGeneration publication;
  mutable std::mutex mutex;
  const RosBooleanOutputAddress address;
  const core::ResourceId resource;
  std::shared_ptr<GpioTopic> topic;
  std::variant<Client::SharedPtr, GpioPublisher::SharedPtr> transport;
  std::vector<Gate> gates;
  std::vector<Alias> aliases;
  core::Duration ack_timeout_ns{};
  bool retired = false;
};

// This bounded table is the provider write model, not another membership or
// eligibility registry. All GPIO mutations hold channel mutex then topic mutex.
struct RosBooleanOutputProvider::GpioTopic final {
  struct Model final {
    std::weak_ptr<State> channel;
    std::optional<State::Written> written;
  };
  rclcpp::Node::SharedPtr node;
  std::string name;
  GpioPublisher::SharedPtr publisher;
  std::mutex mutex;
  std::map<core::ResourceId, Model> model;
};
bool RosBooleanOutputProvider::State::TopicReady() const {
  return topic && topic->publisher &&
         topic->node->get_node_base_interface()->get_context()->is_valid();
}
std::unique_lock<std::mutex>
RosBooleanOutputProvider::State::LockTopic() const {
  return topic ? std::unique_lock<std::mutex>(topic->mutex)
               : std::unique_lock<std::mutex>();
}

Wire RosBooleanOutputProvider::State::BuildWire(
    const std::shared_ptr<State> &self, const bool value, const Gate &gate,
    std::vector<std::shared_ptr<State>> &channel_pins,
    std::vector<std::shared_ptr<const void>> &writer_pins,
    std::optional<Written> &committed_model) {
  if (!self->topic) {
    auto message = std::make_shared<Service::Request>();
    message->data = value;
    return message;
  }
  std::map<std::string, std::map<std::string, bool>> groups;
  // Allocate capacity before obtaining any other actual lifetime pins.
  // Every successful pin survives both locks, including ineligible owners
  // and all exceptional wire-preparation paths.
  channel_pins.reserve(self->topic->model.size());
  writer_pins.reserve(self->topic->model.size());
  for (auto row = self->topic->model.begin(); row != self->topic->model.end();) {
    auto actual = row->second.channel.lock();
    if (!actual) {
      row = self->topic->model.erase(row);
      continue;
    }
    channel_pins.push_back(actual);
    std::optional<bool> written_value;
    if (actual == self)
      written_value = value;
    else if (actual->publication == self->publication && !actual->retired &&
             row->second.written) {
      const auto &written = *row->second.written;
      writer_pins.push_back(written.actual_driver_owner.lock());
      const bool permitted =
          writer_pins.back() &&
          std::any_of(actual->gates.begin(), actual->gates.end(), [&](const auto &candidate) {
            return candidate.accepting && !candidate.terminal &&
                   candidate.provider == written.provider &&
                   SameIoOutputDriverOwner(candidate.actual_driver_owner,
                                           written.actual_driver_owner);
          });
      if (permitted) written_value = written.value;
    }
    if (written_value) {
      const auto &selected = std::get<RosGpioOutputAddress>(actual->address);
      groups[selected.group].emplace(selected.interface_name, *written_value);
    }
    ++row;
  }
  const auto own = self->topic->model.find(self->resource);
  if (own == self->topic->model.end() ||
      !SameIoOutputDriverOwner(own->second.channel, std::weak_ptr<State>(self)))
    throw std::invalid_argument("GPIO model channel is no longer installed");
  GpioMessage message;
  for (const auto &[group, interfaces] : groups) {
    message.interface_groups.push_back(group);
    control_msgs::msg::InterfaceValue values;
    for (const auto &[name, interface_value] : interfaces) {
      values.interface_names.push_back(name);
      values.values.push_back(interface_value ? 1.0 : 0.0);
    }
    message.interface_values.push_back(std::move(values));
  }
  committed_model = Written{value, gate.provider, gate.actual_driver_owner};
  return message;
}

struct RosBooleanOutputProvider::Entities final {
  rclcpp::Node::SharedPtr node;
  rclcpp::CallbackGroup::SharedPtr callback_group;
};

core::Result<std::unique_ptr<RosBooleanOutputProvider>>
RosBooleanOutputProvider::Create(rclcpp::Node::SharedPtr node,
                                 RosBooleanOutputChannel channel,
                                 std::shared_ptr<GpioTopic> topic) {
  using Reply = core::Result<std::unique_ptr<RosBooleanOutputProvider>>;
  const bool address_valid = std::visit(
      [](const auto &address) {
        using Address = std::decay_t<decltype(address)>;
        if constexpr (std::is_same_v<Address, RosGpioOutputAddress>) {
          const auto selector_valid = [](const std::string &text) {
            return !text.empty() && text.size() <= 4096U &&
                   text.find('\0') == std::string::npos;
          };
          return absolute_service(address.topic) &&
                 selector_valid(address.group) &&
                 selector_valid(address.interface_name);
        } else
          return absolute_service(address.service);
      },
      channel.address);
  if (!node || channel.resource_id.empty() || !address_valid ||
      (topic &&
       !std::holds_alternative<RosGpioOutputAddress>(channel.address)) ||
      channel.aliases.empty() ||
      !channel.active_publication_generation.nonzero() ||
      (std::holds_alternative<RosSetBoolOutputAddress>(channel.address) &&
       channel.ack_timeout_ns <= 0))
    return reject<std::unique_ptr<RosBooleanOutputProvider>>(
        "ros_output_configuration_invalid",
        "ROS output requires a resolved service and actual host channel");
  try {
    auto aliases = State::PrepareAliases(channel.aliases);
    if (!aliases.ok())
      return Reply::Err(aliases.error());
    auto state =
        std::make_shared<State>(channel.active_publication_generation,
                                channel.address, channel.resource_id);
    state->gates = std::move(aliases.value().gates);
    state->aliases = std::move(aliases.value().aliases);
    state->ack_timeout_ns = channel.ack_timeout_ns;
    auto entities = std::make_unique<Entities>();
    entities->node = std::move(node);
    entities->callback_group = entities->node->create_callback_group(
        rclcpp::CallbackGroupType::MutuallyExclusive);
    const bool exact_address = std::visit(
        [&](const auto &address) {
          using Address = std::decay_t<decltype(address)>;
          if constexpr (std::is_same_v<Address, RosGpioOutputAddress>) {
            if (!topic) {
              topic = std::make_shared<GpioTopic>();
              topic->node = entities->node;
              topic->name = address.topic;
              topic->publisher = entities->node->create_publisher<GpioMessage>(
                  address.topic, rclcpp::SystemDefaultsQoS());
            }
            if (topic->name != address.topic ||
                topic->node->get_node_base_interface()->get_context() !=
                    entities->node->get_node_base_interface()->get_context())
              throw std::invalid_argument(
                  "GPIO topic pin belongs to another context/address");
            state->topic = topic;
            state->transport = topic->publisher;
            return address.topic == topic->publisher->get_topic_name() &&
                   entities->node->get_node_topics_interface()
                           ->resolve_topic_name(address.topic) == address.topic;
          } else {
            auto client = entities->node->create_client<Service>(
                address.service, rclcpp::ServicesQoS(),
                entities->callback_group);
            const bool exact = address.service == client->get_service_name();
            state->transport = std::move(client);
            return exact;
          }
        },
        channel.address);
    if (!exact_address)
      return reject<std::unique_ptr<RosBooleanOutputProvider>>(
          "ros_output_service_remapped",
          "ROS output address differs from the sealed address");
    if (topic) {
      auto lock = state->LockTopic();
      for (auto row = topic->model.begin(); row != topic->model.end();) {
        const auto actual = row->second.channel.lock();
        if (!actual) {
          row = topic->model.erase(row);
          continue;
        }
        const auto &selected = std::get<RosGpioOutputAddress>(actual->address);
        const auto &requested = std::get<RosGpioOutputAddress>(state->address);
        if (row->first == channel.resource_id ||
            (selected.group == requested.group &&
             selected.interface_name == requested.interface_name))
          return reject<std::unique_ptr<RosBooleanOutputProvider>>(
              "ros_output_gpio_channel_duplicate",
              "GPIO channel already has an actual provider");
        ++row;
      }
      topic->model.emplace(channel.resource_id,
                           GpioTopic::Model{state, std::nullopt});
    }
    return Reply::Ok(std::unique_ptr<RosBooleanOutputProvider>(
        new RosBooleanOutputProvider(std::move(state), std::move(entities))));
  } catch (const std::exception &error) {
    return Reply::Err({"io_ros_output_setup_failed", error.what(), {}});
  }
}

RosBooleanOutputProvider::RosBooleanOutputProvider(
    std::shared_ptr<State> state, std::unique_ptr<Entities> entities)
    : state_(std::move(state)), entities_(std::move(entities)) {}
std::shared_ptr<RosBooleanOutputProvider::GpioTopic>
RosBooleanOutputProvider::gpio_topic() const noexcept {
  return state_->topic;
}

RosBooleanOutputProvider::~RosBooleanOutputProvider() { Shutdown(); }

SetOutputResult RosBooleanOutputProvider::WriteOutput(const core::IoSignalId &signal,
                                                     const bool value,
                                                     const std::stop_token stop) {
  std::shared_ptr<const void> driver_pin;
  std::vector<std::shared_ptr<State>> channel_pins;
  std::vector<std::shared_ptr<const void>> writer_pins;
  Wire wire;
  {
    std::lock_guard lock(state_->mutex);
    auto topic_lock = state_->LockTopic();
    const auto alias = std::find_if(state_->aliases.begin(), state_->aliases.end(),
                                    [&](const auto &row) { return row.signal_id == signal; });
    if (alias == state_->aliases.end())
      return write_failed("io_signal_unknown", "'" + signal.name + "' is not an output of this channel");
    const auto &gate = state_->gates[alias->gate];
    driver_pin = gate.actual_driver_owner.lock();
    if (state_->retired || !driver_pin || !gate.accepting)
      return write_failed("io_output_not_submitted",
                          "the output's I/O component is not enabled; nothing was sent");
    if (!state_->Ready())
      return write_failed("io_output_not_submitted",
                          "the output's service is not available; nothing was sent");
    std::optional<State::Written> committed_model;
    try {
      wire = State::BuildWire(state_, value, gate, channel_pins, writer_pins, committed_model);
    } catch (const std::exception &) {
      return write_failed("io_output_not_submitted",
                          "the output write could not be prepared; nothing was sent");
    }
    if (auto *message = std::get_if<GpioMessage>(&wire)) {
      // A GPIO publish has no acknowledgement or readback.
      try {
        std::get<GpioPublisher::SharedPtr>(state_->transport)->publish(*message);
      } catch (const std::exception &) {
        state_->topic->model.at(state_->resource).written.reset();
        return write_failed("io_output_acceptance_unknown",
                            "the output publish's outcome is unknown",
                            core::ActionOutcome::kUnknown);
      }
      state_->topic->model.at(state_->resource).written = std::move(committed_model);
      return {core::ActionOutcome::kSucceeded, std::nullopt};
    }
  }
  // One request, its answer awaited outside the channel lock.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::nanoseconds(state_->ack_timeout_ns);
  const auto await = [&](const auto &client, const auto &request) -> SetOutputResult {
    auto future = client->async_send_request(request);
    while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
      if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
        client->remove_pending_request(future);
        return write_failed(stop.stop_requested() ? "component_action_interrupted_by_stop"
                                                  : "io_output_acceptance_unknown",
                            "the output write got no answer; its outcome is unknown",
                            core::ActionOutcome::kUnknown);
      }
    }
    const auto response = future.get();
    if (!response)
      return write_failed("io_output_acceptance_unknown",
                          "the output write got no answer; its outcome is unknown",
                          core::ActionOutcome::kUnknown);
    if (!response->success)
      return write_failed("io_output_source_rejected", "the output's service refused the write");
    return {core::ActionOutcome::kSucceeded, std::nullopt};
  };
  try {
    return await(std::get<Client::SharedPtr>(state_->transport),
                 std::get<std::shared_ptr<Service::Request>>(wire));
  } catch (const std::exception &) {
    // A send that threw may still have left: unknown, never resent.
    return write_failed("io_output_acceptance_unknown",
                        "the output write's outcome is unknown", core::ActionOutcome::kUnknown);
  }
}

bool RosBooleanOutputProvider::ServiceReady() const {
  std::lock_guard lock(state_->mutex);
  if (state_->retired)
    return false;
  try {
    return state_->Ready();
  } catch (const std::exception &) {
    return false;
  }
}

core::Result<void> RosBooleanOutputProvider::Pause(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    std::uint64_t generation,
    const std::weak_ptr<const void> &actual_driver_owner) {
  const auto driver_pin = actual_driver_owner.lock();
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  const auto gate = std::find_if(
      state_->gates.begin(), state_->gates.end(), [&](const auto &row) {
        return row.provider == provider &&
               SameIoOutputDriverOwner(row.actual_driver_owner,
                                       actual_driver_owner);
      });
  if (!driver_pin || state_->retired || gate == state_->gates.end() ||
      gate->terminal || generation == 0U || generation < gate->generation ||
      (generation == gate->generation && gate->accepting))
    return reject<void>("io_output_driver_generation_stale",
                        "ROS output Disable is stale or retired");
  gate->generation = generation;
  gate->accepting = false;
  return core::Result<void>::Ok();
}

core::Result<void> RosBooleanOutputProvider::Resume(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    std::uint64_t generation,
    const std::weak_ptr<const void> &actual_driver_owner) {
  const auto driver_pin = actual_driver_owner.lock();
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  const auto gate = std::find_if(
      state_->gates.begin(), state_->gates.end(), [&](const auto &row) {
        return row.provider == provider &&
               SameIoOutputDriverOwner(row.actual_driver_owner,
                                       actual_driver_owner);
      });
  if (!driver_pin || state_->retired || gate == state_->gates.end() ||
      gate->terminal || generation == 0U || generation < gate->generation ||
      (generation == gate->generation && !gate->accepting))
    return reject<void>("io_output_driver_generation_stale",
                        "ROS output Enable is stale or retired");
  gate->generation = generation;
  gate->accepting = true;
  return core::Result<void>::Ok();
}

core::Result<void> RosBooleanOutputProvider::ShutdownOccurrence(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    const std::weak_ptr<const void> &actual_driver_owner) {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  const auto gate = std::find_if(
      state_->gates.begin(), state_->gates.end(), [&](const auto &row) {
        return row.provider == provider &&
               SameIoOutputDriverOwner(row.actual_driver_owner,
                                       actual_driver_owner);
      });
  if (gate == state_->gates.end())
    return reject<void>(
        "io_output_provider_mismatch",
        "Terminal fence does not name a configured output occurrence");
  gate->accepting = false;
  gate->terminal = true;
  return core::Result<void>::Ok();
}

IoOutputAliasState RosBooleanOutputProvider::ReadAliasState() const {
  std::lock_guard lock(state_->mutex);
  std::vector<IoOutputAliasIdentity> aliases;
  aliases.reserve(state_->aliases.size());
  for (const auto &alias : state_->aliases) {
    const auto &gate = state_->gates[alias.gate];
    aliases.push_back(
        {alias.signal_id, gate.provider, gate.actual_driver_owner});
  }
  return {state_->publication, std::move(aliases)};
}

core::Result<void> RosBooleanOutputProvider::RebindAliases(
    const std::vector<IoOutputAliasIdentity> &expected_current_aliases,
    const std::vector<RosBooleanOutputAlias> &candidate_aliases,
    core::ActiveBundlePublicationGeneration publication) {
  if (!publication.nonzero())
    return reject<void>(
        "io_output_publication_invalid",
        "Output alias replacement requires the actual publication");
  auto prepared = State::PrepareAliases(candidate_aliases);
  if (!prepared.ok())
    return core::Result<void>::Err(prepared.error());
  auto &table = prepared.value();
  // Release any final actual owner only after the channel mutex is unlocked;
  // its destructor may perform its exact occurrence fence on this channel.
  std::vector<std::shared_ptr<const void>> driver_pins;
  driver_pins.reserve(table.gates.size());
  for (const auto &gate : table.gates) {
    auto owner = gate.actual_driver_owner.lock();
    if (!owner)
      return reject<void>("io_output_driver_retired",
                          "Candidate actual driver is retired");
    driver_pins.push_back(std::move(owner));
  }
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  if (state_->retired || publication < state_->publication)
    return reject<void>("io_output_alias_rebind_stale",
                        "Output channel or publication is retired");
  if (publication == state_->publication) {
    if (state_->Matches(table))
      return core::Result<void>::Ok();
    return reject<void>("io_output_publication_conflict",
                        "The same publication cannot replace output aliases");
  }
  if (!state_->Matches(expected_current_aliases))
    return reject<void>("io_output_alias_predecessor_mismatch",
                        "The complete actual predecessor alias table changed");
  for (auto &gate : table.gates) {
    const auto retained = std::find_if(
        state_->gates.begin(), state_->gates.end(), [&](const auto &old) {
          return old.provider == gate.provider &&
                 SameIoOutputDriverOwner(old.actual_driver_owner,
                                         gate.actual_driver_owner);
        });
    if (retained != state_->gates.end())
      gate = *retained;
  }
  state_->gates.swap(table.gates);
  state_->aliases.swap(table.aliases);
  state_->publication = publication;
  return core::Result<void>::Ok();
}

void RosBooleanOutputProvider::Shutdown() {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  if (state_->topic) {
    const auto own = state_->topic->model.find(state_->resource);
    if (own != state_->topic->model.end() &&
        SameIoOutputDriverOwner(own->second.channel,
                                std::weak_ptr<State>(state_)))
      state_->topic->model.erase(own);
  }
  state_->retired = true;
  for (auto &gate : state_->gates)
    gate.accepting = false;
}

} // namespace manyforge::roboplan_backend
