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
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <map>
#include <mutex>
#include <rclcpp/client.hpp>
#include <rclcpp/publisher.hpp>
#include <rclcpp/qos.hpp>
#include <set>
#include <std_srvs/srv/set_bool.hpp>
#include <type_traits>
#include <ur_msgs/srv/set_io.hpp>
#include <utility>
#include <xarm_msgs/srv/set_digital_io.hpp>

namespace manyforge::roboplan_backend {
namespace {
using Service = std_srvs::srv::SetBool;
using Client = rclcpp::Client<Service>;
using UrService = ur_msgs::srv::SetIO;
using UrClient = rclcpp::Client<UrService>;
using XArmService = xarm_msgs::srv::SetDigitalIO;
using XArmClient = rclcpp::Client<XArmService>;
using GpioMessage = control_msgs::msg::DynamicInterfaceGroupValues;
using GpioPublisher = rclcpp::Publisher<GpioMessage>;

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

bool valid_identity(const core::IoOutputCommandIdentityDTO &identity) {
  const auto &command = identity.command;
  return !command.operation_id.empty() &&
         command.operation_revision.nonzero() &&
         !command.participant_id.empty() &&
         !command.command_id.empty() &&
         command.command_generation.nonzero() &&
         identity.component_lifecycle_generation.nonzero() &&
         core::IsExactLowercaseSha256Digest(
             identity.prepared_contract_digest.value()) &&
         identity.provider.locally_valid();
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
  struct Record final {
    core::IoOutputWriteRequestDTO request;
    core::IoOutputObservationDTO observation;
    bool group_model_excluded = false;
    core::ErrorDTO unknown_error{
        "io_output_acceptance_unknown",
        "A possible SetBool send has no authoritative acceptance result",
        {}};
    core::ErrorDTO rejected_error{"io_output_source_rejected",
                                  "SetBool source rejected this request",
                                  {}};
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
  State(core::IClock &clock,
        core::ActiveBundlePublicationGeneration publication,
        RosBooleanOutputAddress address, core::ResourceId resource)
      : publication(publication), clock(clock), address(std::move(address)),
        resource(std::move(resource)) {}
  bool SubmittedOnly() const {
    return std::holds_alternative<RosGpioOutputAddress>(address);
  }
  bool TopicReady() const;
  std::unique_lock<std::mutex> LockTopic() const;
  bool Ready() const {
    if (SubmittedOnly())
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
  template <class Future>
  static void ReceiveResponse(std::weak_ptr<State> weak_state,
                              std::weak_ptr<Record> weak_record,
                              Future future) {
    const auto state = weak_state.lock();
    if (!state)
      return;
    const auto received_at = state->clock.now();
    const auto record = weak_record.lock();
    if (!record)
      return;
    std::optional<bool> accepted;
    std::optional<std::int32_t> vendor_return;
    try {
      const auto response = future.get();
      if (response) {
        if constexpr (requires { response->ret; }) {
          // The vendor contract does not prove nonzero returns had no effect.
          if (response->ret == 0)
            accepted = true;
          else
            vendor_return = response->ret;
        } else {
          accepted = response->success;
        }
      }
    } catch (const std::exception &) {
    }
    std::lock_guard lock(state->mutex);
    if (state->record != record ||
        (record->observation.evidence != core::IoOutputEvidence::kSubmitted &&
         record->observation.evidence !=
             core::IoOutputEvidence::kAcceptanceUnknown))
      return;
    record->observation.trusted_received_at_ns = received_at;
    if (!accepted) {
      record->observation.evidence = core::IoOutputEvidence::kAcceptanceUnknown;
      if (!record->observation.error)
        record->observation.error = std::move(record->unknown_error);
      if (vendor_return) {
        record->observation.error->details["reason"] =
            "xarm_service_returned_nonzero";
        record->observation.error->details["vendor_return"] =
            std::to_string(*vendor_return);
      }
    } else if (*accepted) {
      record->observation.evidence = core::IoOutputEvidence::kSourceAccepted;
      record->observation.error.reset();
    } else {
      record->observation.evidence = core::IoOutputEvidence::kSourceRejected;
      record->observation.error = std::move(record->rejected_error);
    }
  }
  core::ActiveBundlePublicationGeneration publication;
  core::IClock &clock;
  mutable std::mutex mutex;
  const RosBooleanOutputAddress address;
  const core::ResourceId resource;
  std::shared_ptr<GpioTopic> topic;
  std::variant<Client::SharedPtr, UrClient::SharedPtr, GpioPublisher::SharedPtr,
               XArmClient::SharedPtr> transport;
  std::vector<Gate> gates;
  std::vector<Alias> aliases;
  std::shared_ptr<Record> record;
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

struct RosBooleanOutputProvider::Entities final {
  rclcpp::Node::SharedPtr node;
  rclcpp::CallbackGroup::SharedPtr callback_group;
};

core::Result<std::unique_ptr<RosBooleanOutputProvider>>
RosBooleanOutputProvider::Create(rclcpp::Node::SharedPtr node,
                                 core::IClock &clock,
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
        } else if constexpr (std::is_same_v<Address, UrSetIoOutputAddress>)
          return absolute_service(address.service) && address.pin <= 17U;
        else if constexpr (std::is_same_v<Address, XArmSetDigitalOutputAddress>)
          return absolute_service(address.service) &&
                 ((address.bank == XArmDigitalBank::kTool && address.pin <= 4U) ||
                  (address.bank == XArmDigitalBank::kController &&
                   address.pin <= 15U));
        else
          return absolute_service(address.service);
      },
      channel.address);
  if (!node || channel.resource_id.empty() || !address_valid ||
      (topic &&
       !std::holds_alternative<RosGpioOutputAddress>(channel.address)) ||
      channel.aliases.empty() ||
      !channel.active_publication_generation.nonzero())
    return reject<std::unique_ptr<RosBooleanOutputProvider>>(
        "ros_output_configuration_invalid",
        "ROS output requires a resolved service and actual host channel");
  try {
    auto aliases = State::PrepareAliases(channel.aliases);
    if (!aliases.ok())
      return Reply::Err(aliases.error());
    auto state =
        std::make_shared<State>(clock, channel.active_publication_generation,
                                channel.address, channel.resource_id);
    state->gates = std::move(aliases.value().gates);
    state->aliases = std::move(aliases.value().aliases);
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
          } else if constexpr (std::is_same_v<Address, UrSetIoOutputAddress>) {
            auto client = entities->node->create_client<UrService>(
                address.service, rclcpp::ServicesQoS(),
                entities->callback_group);
            const bool exact = address.service == client->get_service_name();
            state->transport = std::move(client);
            return exact;
          } else if constexpr (std::is_same_v<Address, XArmSetDigitalOutputAddress>) {
            auto client = entities->node->create_client<XArmService>(
                address.service, rclcpp::ServicesQoS(), entities->callback_group);
            const bool exact = address.service == client->get_service_name();
            state->transport = std::move(client);
            return exact;
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

core::IoOutputEvidence
RosBooleanOutputProvider::StrongestOutputEvidence() const noexcept {
  return state_->SubmittedOnly() ? core::IoOutputEvidence::kSubmitted
                                 : core::IoOutputEvidence::kSourceAccepted;
}

core::Result<core::IoOutputObservationDTO>
RosBooleanOutputProvider::SubmitOutput(
    const core::IoOutputWriteRequestDTO &request) {
  using Reply = core::Result<core::IoOutputObservationDTO>;
  std::shared_ptr<const void> driver_pin;
  std::vector<std::shared_ptr<State>> channel_pins;
  std::vector<std::shared_ptr<const void>> writer_pins;
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  if (state_->record) {
    if (state_->record->request == request)
      return Reply::Ok(state_->record->observation);
    return reject<core::IoOutputObservationDTO>(
        state_->record->request.identity == request.identity
            ? "io_output_request_conflict"
            : "io_output_correlation_busy",
        "Existing command owner must retire its exact output correlation");
  }
  if (!valid_identity(request.identity))
    return reject<core::IoOutputObservationDTO>(
        "io_output_identity_invalid",
        "Output requires its actual committed identity");
  const auto alias = std::find_if(
      state_->aliases.begin(), state_->aliases.end(),
      [&](const auto &row) { return row.signal_id == request.signal_id; });
  if (alias == state_->aliases.end() ||
      state_->gates[alias->gate].provider != request.identity.provider)
    return reject<core::IoOutputObservationDTO>(
        "io_output_provider_mismatch",
        "Output alias does not name its configured occurrence");

  driver_pin = state_->gates[alias->gate].actual_driver_owner.lock();
  // The complete record, fixed errors and wire request exist BEFORE any
  // possible effect. A lost return or throwing async send retains honest
  // unknown evidence rather than allowing another send.
  auto record = std::make_shared<State::Record>(State::Record{
      request,
      {request.identity, core::IoOutputEvidence::kNotSubmitted, 0, {}}});
  if (std::holds_alternative<XArmSetDigitalOutputAddress>(state_->address))
    record->unknown_error.message =
        "An xArm digital output send has no confirmed delivery result";
  state_->record = record;
  const auto now = state_->clock.now();
  record->observation.trusted_received_at_ns = now;
  std::string reason;
  if (state_->retired || !driver_pin || !state_->gates[alias->gate].accepting)
    reason = "io_output_driver_disabled";
  else if (state_->SubmittedOnly() && request.absolute_deadline_ns)
    reason = "io_output_submission_deadline_not_supported";
  else if (!state_->SubmittedOnly() && !request.absolute_deadline_ns)
    reason = "io_output_deadline_missing";
  else if (request.absolute_deadline_ns &&
           (*request.absolute_deadline_ns <= 0 ||
            now >= *request.absolute_deadline_ns))
    reason = "io_output_deadline_expired";
  else {
    try {
      if (!state_->Ready())
        reason = "io_output_service_unavailable";
    } catch (const std::exception &) {
      reason = "io_output_service_unavailable";
    }
  }
  if (!reason.empty()) {
    record->observation.error =
        core::ErrorDTO{"io_output_not_submitted",
                       std::holds_alternative<XArmSetDigitalOutputAddress>(
                           state_->address)
                           ? "xArm digital output was refused before any send"
                           : "SetBool was refused before any send",
                       {{"reason", std::move(reason)}}};
    return Reply::Ok(record->observation);
  }
  // Complete the typed wire value before the final deadline/effect fence.
  // A failure here proves no send and must not become an unknown acceptance.
  using Wire = std::variant<std::shared_ptr<Service::Request>,
                            std::shared_ptr<UrService::Request>, GpioMessage,
                            std::shared_ptr<XArmService::Request>>;
  Wire wire;
  std::optional<State::Written> committed_model;
  try {
    if (const auto *address =
            std::get_if<UrSetIoOutputAddress>(&state_->address)) {
      auto message = std::make_shared<UrService::Request>();
      message->fun = UrService::Request::FUN_SET_DIGITAL_OUT;
      message->pin = static_cast<std::int8_t>(address->pin);
      message->state = request.value ? 1.0F : 0.0F;
      wire = std::move(message);
    } else if (const auto *address =
                   std::get_if<XArmSetDigitalOutputAddress>(&state_->address)) {
      auto message = std::make_shared<XArmService::Request>();
      message->ionum = static_cast<std::int16_t>(address->pin);
      message->value = request.value ? 1 : 0;
      message->delay_sec = 0.0F;
      wire = std::move(message);
    } else if (state_->topic) {
      std::map<std::string, std::map<std::string, bool>> groups;
      // Allocate capacity before obtaining any other actual lifetime pins.
      // Every successful pin survives both locks, including ineligible owners
      // and all exceptional wire-preparation paths.
      channel_pins.reserve(state_->topic->model.size());
      writer_pins.reserve(state_->topic->model.size());
      for (auto row = state_->topic->model.begin();
           row != state_->topic->model.end();) {
        auto actual = row->second.channel.lock();
        if (!actual) {
          row = state_->topic->model.erase(row);
          continue;
        }
        channel_pins.push_back(actual);
        std::optional<bool> value;
        if (actual == state_)
          value = request.value;
        else if (actual->publication == state_->publication &&
                 !actual->retired && row->second.written &&
                 (!actual->record ||
                  (!actual->record->group_model_excluded &&
                   actual->record->observation.evidence !=
                       core::IoOutputEvidence::kAcceptanceUnknown))) {
          const auto &written = *row->second.written;
          writer_pins.push_back(written.actual_driver_owner.lock());
          const bool permitted =
              writer_pins.back() &&
              std::any_of(actual->gates.begin(), actual->gates.end(),
                          [&](const auto &gate) {
                            return gate.accepting && !gate.terminal &&
                                   gate.provider == written.provider &&
                                   SameIoOutputDriverOwner(
                                       gate.actual_driver_owner,
                                       written.actual_driver_owner);
                          });
          if (permitted) {
            value = written.value;
          }
        }
        if (value) {
          const auto &selected =
              std::get<RosGpioOutputAddress>(actual->address);
          groups[selected.group].emplace(selected.interface_name, *value);
        }
        ++row;
      }
      const auto own = state_->topic->model.find(state_->resource);
      if (own == state_->topic->model.end() ||
          !SameIoOutputDriverOwner(own->second.channel,
                                   std::weak_ptr<State>(state_)))
        throw std::invalid_argument(
            "GPIO model channel is no longer installed");
      GpioMessage message;
      for (const auto &[group, interfaces] : groups) {
        message.interface_groups.push_back(group);
        control_msgs::msg::InterfaceValue values;
        for (const auto &[name, value] : interfaces) {
          values.interface_names.push_back(name);
          values.values.push_back(value ? 1.0 : 0.0);
        }
        message.interface_values.push_back(std::move(values));
      }
      const auto &gate = state_->gates[alias->gate];
      committed_model = State::Written{request.value, gate.provider,
                                       gate.actual_driver_owner};
      wire = std::move(message);
    } else {
      auto message = std::make_shared<Service::Request>();
      message->data = request.value;
      wire = std::move(message);
    }
  } catch (const std::exception &) {
    record->observation.error =
        core::ErrorDTO{"io_output_not_submitted",
                       "Output wire preparation failed before any send",
                       {{"reason", "io_output_wire_preparation_failed"}}};
    return Reply::Ok(record->observation);
  }
  const auto weak_state = std::weak_ptr<State>(state_);
  const auto weak_record = std::weak_ptr<State::Record>(record);
  const auto send_at = state_->clock.now();
  record->observation.trusted_received_at_ns = send_at;
  if (request.absolute_deadline_ns &&
      send_at >= *request.absolute_deadline_ns) {
    record->observation.error =
        core::ErrorDTO{"io_output_not_submitted",
                       "Output deadline elapsed before any send",
                       {{"reason", "io_output_deadline_expired"}}};
    return Reply::Ok(record->observation);
  }
  record->observation.evidence = core::IoOutputEvidence::kSubmitted;
  try {
    std::visit(
        [&](const auto &message) {
          using Message = std::decay_t<decltype(message)>;
          if constexpr (std::is_same_v<Message,
                                       std::shared_ptr<Service::Request>>) {
            (void)std::get<Client::SharedPtr>(state_->transport)
                ->async_send_request(message, [weak_state, weak_record](
                                                  Client::SharedFuture future) {
                  State::ReceiveResponse(weak_state, weak_record,
                                         std::move(future));
                });
          } else if constexpr (std::is_same_v<
                                   Message,
                                   std::shared_ptr<UrService::Request>>) {
            (void)std::get<UrClient::SharedPtr>(state_->transport)
                ->async_send_request(
                    message,
                    [weak_state, weak_record](UrClient::SharedFuture future) {
                      State::ReceiveResponse(weak_state, weak_record,
                                             std::move(future));
                    });
          } else if constexpr (std::is_same_v<
                                   Message,
                                   std::shared_ptr<XArmService::Request>>) {
            (void)std::get<XArmClient::SharedPtr>(state_->transport)
                ->async_send_request(
                    message,
                    [weak_state, weak_record](XArmClient::SharedFuture future) {
                      State::ReceiveResponse(weak_state, weak_record,
                                             std::move(future));
                    });
          } else
            std::get<GpioPublisher::SharedPtr>(state_->transport)
                ->publish(message);
        },
        wire);
    if (state_->topic) {
      static_assert(
          std::is_nothrow_move_assignable_v<decltype(committed_model)>);
      state_->topic->model.at(state_->resource).written =
          std::move(committed_model);
    }
  } catch (...) {
    if (state_->topic) {
      const auto own = state_->topic->model.find(state_->resource);
      if (own != state_->topic->model.end() &&
          SameIoOutputDriverOwner(own->second.channel,
                                  std::weak_ptr<State>(state_)))
        own->second.written.reset();
    }
    // rclcpp may throw AFTER rcl_send_request but before recording/returning
    // its request ID. Keep unknown; do not retry or claim not-submitted.
    record->observation.evidence = core::IoOutputEvidence::kAcceptanceUnknown;
    record->observation.error = std::move(record->unknown_error);
  }
  return Reply::Ok(record->observation);
}

core::Result<core::IoOutputObservationDTO>
RosBooleanOutputProvider::ObserveOutputResult(
    const core::IoOutputCommandIdentityDTO &identity) const {
  std::lock_guard lock(state_->mutex);
  if (!state_->record || state_->record->request.identity != identity)
    return reject<core::IoOutputObservationDTO>(
        "io_output_correlation_unavailable",
        "No retained evidence for this exact output command");
  return core::Result<core::IoOutputObservationDTO>::Ok(
      state_->record->observation);
}

core::Result<void> RosBooleanOutputProvider::ExcludeFromGroupModel(
    const core::IoOutputCommandIdentityDTO &identity) {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  if (!state_->topic)
    return core::Result<void>::Ok();
  if (!state_->record || state_->record->request.identity != identity)
    return reject<void>("io_output_correlation_mismatch",
                        "Exclusion requires the exact retained command");
  // The engine retains uncertainty only after this same-topic fence returns.
  // Keep the actual transport observation and correlation intact.
  state_->record->group_model_excluded = true;
  const auto own = state_->topic->model.find(state_->resource);
  if (own != state_->topic->model.end() &&
      SameIoOutputDriverOwner(own->second.channel,
                              std::weak_ptr<State>(state_)))
    own->second.written.reset();
  return core::Result<void>::Ok();
}

core::Result<void> RosBooleanOutputProvider::RetireOutput(
    const core::IoOutputCommandIdentityDTO &identity) {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  if (state_->record && state_->record->request.identity != identity)
    return reject<void>("io_output_correlation_mismatch",
                        "Retirement cannot clear another command");
  if (state_->topic && state_->record &&
      (state_->record->group_model_excluded ||
       state_->record->observation.evidence ==
           core::IoOutputEvidence::kAcceptanceUnknown)) {
    const auto own = state_->topic->model.find(state_->resource);
    if (own != state_->topic->model.end() &&
        SameIoOutputDriverOwner(own->second.channel,
                                std::weak_ptr<State>(state_)))
      own->second.written.reset();
  }
  state_->record.reset();
  // This client is private to the single bounded record. Pruning also covers
  // a send that threw before returning an ID. It is local correlation cleanup,
  // not cancellation of a remote call or evidence that it had no effect.
  std::visit(
      [](const auto &channel) {
        using Channel = std::decay_t<decltype(channel)>;
        if constexpr (!std::is_same_v<Channel, GpioPublisher::SharedPtr>)
          (void)channel->prune_pending_requests();
      },
      state_->transport);
  return core::Result<void>::Ok();
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
  if (!state_->SubmittedOnly() && state_->record &&
      state_->record->observation.evidence ==
          core::IoOutputEvidence::kSubmitted) {
    state_->record->observation.evidence =
        core::IoOutputEvidence::kAcceptanceUnknown;
    state_->record->observation.error =
        std::move(state_->record->unknown_error);
  }
  // Keep the one pending correlation while this provider lives: an actual ACK
  // remains truthful after shutdown. Only exact owner Retire fences it; object
  // destruction also releases the private client after host callback
  // quiescence.
}

} // namespace manyforge::roboplan_backend
