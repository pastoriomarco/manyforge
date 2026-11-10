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
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <manyforge_ros2_control_execution/graph_event_observer.hpp>
#include <manyforge_ros2_control_execution/detail/leaf_timing.hpp>
#include "cached_ros_context.hpp"
#include <atomic>
#include <manyforge_core/types/typed_error.hpp>

#include <algorithm>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <future>
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

// The specific refusal code is the typed error code; it is not wrapped in a
// generic umbrella code.
template <class T>
core::Result<T> reject(std::string reason, std::string message) {
  return core::Result<T>::Err({std::move(reason), std::move(message), {}});
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
  struct ModelFacts final {
    core::ActiveBundlePublicationGeneration publication;
    bool retired;
    std::vector<Gate> gates;
  };
  void RefreshModelFacts() {
    model_facts.store(std::make_shared<const ModelFacts>(ModelFacts{publication, retired, gates}),
                      std::memory_order_release);
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
  std::unique_lock<std::mutex> LockTopic() const;
  /// Build the aggregate under the exact publisher leaf from atomic channel
  /// facts and that publisher's model. Lifetime pins outlive the leaf. Throws
  /// before SDK entry when the wire cannot be prepared.
  static Wire BuildWire(const std::shared_ptr<State> &self, bool value, const Gate &gate,
                        const std::shared_ptr<void> &publisher_leaf,
                        const std::shared_ptr<const ModelFacts> &own_facts,
                        std::vector<std::shared_ptr<State>> &channel_pins,
                        std::vector<std::shared_ptr<const void>> &writer_pins,
                        std::optional<Written> &committed_model);
  core::ActiveBundlePublicationGeneration publication;
  mutable std::mutex mutex;
  std::atomic<std::shared_ptr<const ModelFacts>> model_facts;
  const RosBooleanOutputAddress address;
  const core::ResourceId resource;
  std::shared_ptr<GpioTopic> topic;
  std::variant<Client::SharedPtr, GpioPublisher::SharedPtr> transport;
  std::atomic<std::shared_ptr<std::atomic<int>>> cached_service_ready;
  std::atomic<std::shared_ptr<host_detail::CachedRosContext>> cached_context;
  std::vector<Gate> gates;
  std::vector<Alias> aliases;
  core::Duration ack_timeout_ns{};
  std::weak_ptr<ros2_control_execution::Ros2TransportResources> owner;
  bool retired = false;
};

// This bounded table is the provider write model, not another membership or
// eligibility registry. All GPIO mutations hold channel mutex then topic mutex.
struct RosBooleanOutputProvider::GpioTopic final {
  struct Model final {
    std::weak_ptr<State> channel;
    std::optional<State::Written> written;
  };
  struct TopicEntities final {
    rclcpp::Node::SharedPtr node;
    GpioPublisher::SharedPtr publisher;
    std::weak_ptr<ros2_control_execution::Ros2TransportResources> owner;
    bool owner_required{false};
    // Exact publisher leaf: aggregate and last-written model move together.
    std::mutex send_mutex;
    std::atomic<bool> accepting{true};
    std::map<core::ResourceId, Model> written_model;
  };
  std::shared_ptr<TopicEntities> entities;
  std::string name;
  std::mutex mutex;
  std::map<core::ResourceId, Model> model;
};
std::unique_lock<std::mutex>
RosBooleanOutputProvider::State::LockTopic() const {
  return topic ? std::unique_lock<std::mutex>(topic->mutex)
               : std::unique_lock<std::mutex>();
}

Wire RosBooleanOutputProvider::State::BuildWire(
    const std::shared_ptr<State> &self, const bool value, const Gate &gate,
    const std::shared_ptr<void> &publisher_leaf,
    const std::shared_ptr<const ModelFacts> &own_facts,
    std::vector<std::shared_ptr<State>> &channel_pins,
    std::vector<std::shared_ptr<const void>> &writer_pins,
    std::optional<Written> &committed_model) {
  if (!self->topic) {
    auto message = std::make_shared<Service::Request>();
    message->data = value;
    return message;
  }
  auto leaf = std::static_pointer_cast<GpioTopic::TopicEntities>(publisher_leaf);
  if (!leaf) throw std::invalid_argument("GPIO publisher is unavailable");
  std::map<std::string, std::map<std::string, bool>> groups;
  auto &model = leaf->written_model;
  channel_pins.reserve(model.size());
  writer_pins.reserve(model.size());
  if (!own_facts) throw std::invalid_argument("GPIO channel retired");
  for (auto row = model.begin(); row != model.end();) {
    auto actual = row->second.channel.lock();
    if (!actual) { row = model.erase(row); continue; }
    channel_pins.push_back(actual);
    const auto facts = actual->model_facts.load(std::memory_order_acquire);
    std::optional<bool> written_value;
    if (actual == self) written_value = value;
    else if (facts && facts->publication == own_facts->publication &&
             !facts->retired && row->second.written) {
      const auto &written = *row->second.written;
      writer_pins.push_back(written.actual_driver_owner.lock());
      const bool permitted = writer_pins.back() &&
          std::any_of(facts->gates.begin(), facts->gates.end(), [&](const auto &candidate) {
            return candidate.accepting && !candidate.terminal &&
                   candidate.provider == written.provider &&
                   SameIoOutputDriverOwner(candidate.actual_driver_owner,
                                           written.actual_driver_owner);
          });
      if (permitted) written_value = written.value;
    }
    if (written_value) {
      const auto &selected = std::get<RosGpioOutputAddress>(actual->address);
      groups[selected.group].emplace(selected.interface_name,
                                     *written_value);
    }
    ++row;
  }
  const auto &own_address = std::get<RosGpioOutputAddress>(self->address);
  groups[own_address.group][own_address.interface_name] = value;
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
  Client::SharedPtr client;
  std::weak_ptr<ros2_control_execution::Ros2TransportResources> owner;
  bool owner_required{false};
  std::atomic<bool> accepting{true};
  std::mutex send_mutex;
  std::shared_ptr<host_detail::CachedRosContext> context;
  std::shared_ptr<std::atomic<int>> service_ready;
  std::shared_ptr<ros2_control_execution::GraphEventObserver> service_observer;
  void InstallServiceObserver(const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &owner) {
    service_ready = std::make_shared<std::atomic<int>>(-1);
    auto refresh = [weak = std::weak_ptr<Client>(client), ready = service_ready] {
      try {
        const auto selected = weak.lock();
        ready->store(selected ? static_cast<int>(selected->service_is_ready()) : -1,
                     std::memory_order_release);
      } catch (...) {
        ready->store(-1, std::memory_order_release);
      }
    };
    auto observer = std::make_shared<ros2_control_execution::GraphEventObserver>(
        node, callback_group, refresh);
    service_observer = owner ? owner->retain_entity(std::move(observer)) : std::move(observer);
    refresh();
  }
};

struct RosBooleanOutputProvider::TransportRecoverySnapshot final {
  std::shared_ptr<State> state;
  std::shared_ptr<Entities> entities;
  std::shared_ptr<GpioTopic> topic;
  std::shared_ptr<GpioTopic::TopicEntities> topic_entities;
};
struct RosBooleanOutputProvider::GpioTransportRecoveryStage final {
  std::shared_ptr<GpioTopic> topic;
  std::shared_ptr<GpioTopic::TopicEntities> expected;
  std::shared_ptr<GpioTopic::TopicEntities> entities;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner;
  bool closed = false;
  bool committed = false;
  ~GpioTransportRecoveryStage() {
    if (closed && !committed && expected)
      expected->accepting.store(true, std::memory_order_release);
  }
};
struct RosBooleanOutputProvider::TransportRecoveryStage final {
  std::shared_ptr<const TransportRecoverySnapshot> snapshot;
  std::shared_ptr<Entities> entities;
  std::shared_ptr<GpioTransportRecoveryStage> gpio;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner;
};

std::shared_ptr<const RosBooleanOutputProvider::TransportRecoverySnapshot>
RosBooleanOutputProvider::CaptureTransportRecovery() const {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  return std::make_shared<TransportRecoverySnapshot>(TransportRecoverySnapshot{
      state_, entities_, state_->topic,
      state_->topic ? state_->topic->entities : nullptr});
}
bool RosBooleanOutputProvider::BelongsToTransport(
    const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &owner) const {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  if (!owner)
    return false;
  if (state_->topic)
    return state_->topic->entities &&
           state_->topic->entities->node == owner->node();
  return state_->owner.lock() == owner;
}
bool RosBooleanOutputProvider::MatchesTransportRecovery(
    const TransportRecoverySnapshot &snapshot) const noexcept {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  return state_ == snapshot.state && entities_ == snapshot.entities &&
         state_->topic == snapshot.topic &&
         (!state_->topic || state_->topic->entities == snapshot.topic_entities);
}
core::Result<
    std::shared_ptr<RosBooleanOutputProvider::GpioTransportRecoveryStage>>
RosBooleanOutputProvider::StageGpioTransportRecovery(
    const std::shared_ptr<GpioTopic> &topic,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner) {
  using Reply = core::Result<std::shared_ptr<GpioTransportRecoveryStage>>;
  if (!topic || !owner || !owner->node())
    return reject<std::shared_ptr<GpioTransportRecoveryStage>>(
        "invalid_request", "GPIO recovery requires a topic and owner");
  try {
    auto stage = std::make_shared<GpioTransportRecoveryStage>();
    stage->topic = topic;
    stage->owner = std::move(owner);
    {
      std::lock_guard lock(topic->mutex);
      stage->expected = topic->entities;
    }
    stage->owner->invoke_creation([&] {
      auto entities = std::make_shared<GpioTopic::TopicEntities>();
      entities->node = stage->owner->node();
      entities->owner = stage->owner;
      entities->owner_required = true;
      if (entities->node->get_node_topics_interface()->resolve_topic_name(
              topic->name) != topic->name)
        throw std::invalid_argument(
            "GPIO recovery topic differs from the sealed address");
      entities->publisher = entities->node->create_publisher<GpioMessage>(
          topic->name, rclcpp::SystemDefaultsQoS());
      if (entities->publisher->get_topic_name() != topic->name)
        throw std::invalid_argument(
            "GPIO recovery publisher differs from the sealed address");
      stage->entities = stage->owner->retain_entity(std::move(entities));
    });
    if (!stage->expected) throw std::invalid_argument("GPIO predecessor missing");
    stage->expected->accepting.store(false, std::memory_order_release);
    stage->closed = true;
#ifdef MANYFORGE_LEAF_TIMING
    ros2_control_execution::detail::gpio_recovery_closed.store(true, std::memory_order_release);
#endif
    {
      // Lifecycle worker waits outside the owner callback and all facts locks.
      MANYFORGE_LEAF_LOCK(leaf, stage->expected->send_mutex,
          ros2_control_execution::detail::LeafEntity::kGpioRecoveryCopy);
      stage->entities->written_model = stage->expected->written_model;
    }
    return Reply::Ok(std::move(stage));
  } catch (const core::TypedError &error) {
    return Reply::Err(error.error());
  } catch (const std::exception &error) {
    return Reply::Err({"io_ros_output_setup_failed", error.what(), {}});
  }
}
core::Result<std::shared_ptr<RosBooleanOutputProvider::TransportRecoveryStage>>
RosBooleanOutputProvider::StageTransportRecovery(
    std::shared_ptr<const TransportRecoverySnapshot> snapshot,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner,
    std::shared_ptr<GpioTransportRecoveryStage> gpio) {
  using Reply = core::Result<std::shared_ptr<TransportRecoveryStage>>;
  if (!snapshot || !owner || !owner->node() ||
      (snapshot->topic &&
       (!gpio || gpio->topic != snapshot->topic ||
        gpio->expected != snapshot->topic_entities || gpio->owner != owner)) ||
      (!snapshot->topic && gpio))
    return reject<std::shared_ptr<TransportRecoveryStage>>(
        "invalid_request", "Output recovery snapshot or topic does not match");
  try {
    auto stage = std::make_shared<TransportRecoveryStage>();
    stage->snapshot = std::move(snapshot);
    stage->owner = std::move(owner);
    stage->gpio = std::move(gpio);
    stage->owner->invoke_creation([&] {
      auto entities = std::make_shared<Entities>();
      entities->node = stage->owner->node();
      entities->owner = stage->owner;
      entities->owner_required = true;
      entities->context = std::make_shared<host_detail::CachedRosContext>(
          entities->node->get_node_base_interface()->get_context());
      entities->callback_group = entities->node->create_callback_group(
          rclcpp::CallbackGroupType::MutuallyExclusive);
      if (!stage->gpio) {
        const auto &address =
            std::get<RosSetBoolOutputAddress>(stage->snapshot->state->address);
        entities->client = entities->node->create_client<Service>(
            address.service, rclcpp::ServicesQoS(), entities->callback_group);
        if (entities->client->get_service_name() != address.service)
          throw std::invalid_argument(
              "Recovery service differs from the sealed address");
        entities->InstallServiceObserver(stage->owner);
      }
      stage->entities = stage->owner->retain_entity(std::move(entities));
    });
    return Reply::Ok(std::move(stage));
  } catch (const core::TypedError &error) {
    return Reply::Err(error.error());
  } catch (const std::exception &error) {
    return Reply::Err({"io_ros_output_setup_failed", error.what(), {}});
  }
}
void RosBooleanOutputProvider::CommitGpioTransportRecovery(
    GpioTransportRecoveryStage &stage) noexcept {
  std::lock_guard lock(stage.topic->mutex);
  stage.topic->entities = stage.entities;
  stage.committed = true;
}
void RosBooleanOutputProvider::CommitTransportRecovery(
    TransportRecoveryStage &stage) noexcept {
  std::lock_guard lock(state_->mutex);
  auto topic_lock = state_->LockTopic();
  entities_->accepting.store(false, std::memory_order_release);
  entities_ = stage.entities;
  state_->owner = stage.owner;
  if (stage.gpio)
    state_->transport = GpioPublisher::SharedPtr(
        stage.gpio->entities, stage.gpio->entities->publisher.get());
  else
    state_->transport = Client::SharedPtr(entities_, entities_->client.get());
  state_->cached_service_ready.store(entities_->service_ready, std::memory_order_release);
  state_->cached_context.store(entities_->context, std::memory_order_release);
}

core::Result<std::unique_ptr<RosBooleanOutputProvider>>
RosBooleanOutputProvider::Create(
    rclcpp::Node::SharedPtr node, RosBooleanOutputChannel channel,
    std::shared_ptr<GpioTopic> topic,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner) {
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
  if (!node || !owner || owner->node() != node || channel.resource_id.empty() || !address_valid ||
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
    auto state = std::make_shared<State>(channel.active_publication_generation,
                                         channel.address, channel.resource_id);
    state->gates = std::move(aliases.value().gates);
    state->aliases = std::move(aliases.value().aliases);
    state->RefreshModelFacts();
    state->ack_timeout_ns = channel.ack_timeout_ns;
    state->owner = owner;
    auto entities = std::make_shared<Entities>();
    entities->node = std::move(node);
    entities->owner = owner;
    entities->owner_required = static_cast<bool>(owner);
    if (owner)
      entities = owner->retain_entity(std::move(entities));
    const auto invoke = [&](auto &&work) {
      if (owner)
        owner->invoke_creation(std::forward<decltype(work)>(work));
      else
        work();
    };
    invoke([&] {
      entities->context = std::make_shared<host_detail::CachedRosContext>(
          entities->node->get_node_base_interface()->get_context());
      entities->callback_group = entities->node->create_callback_group(
          rclcpp::CallbackGroupType::MutuallyExclusive);
    });
    const bool exact_address = std::visit(
        [&](const auto &address) {
          using Address = std::decay_t<decltype(address)>;
          if constexpr (std::is_same_v<Address, RosGpioOutputAddress>) {
            if (!topic) {
              topic = std::make_shared<GpioTopic>();
              topic->name = address.topic;
              invoke([&] {
                auto topic_entities =
                    std::make_shared<GpioTopic::TopicEntities>();
                topic_entities->node = entities->node;
                topic_entities->owner = owner;
                topic_entities->owner_required = static_cast<bool>(owner);
                topic_entities->publisher =
                    entities->node->create_publisher<GpioMessage>(
                        address.topic, rclcpp::SystemDefaultsQoS());
                topic->entities =
                    owner ? owner->retain_entity(std::move(topic_entities))
                          : std::move(topic_entities);
              });
            }
            if (topic->name != address.topic ||
                topic->entities->node->get_node_base_interface()
                        ->get_context() !=
                    entities->node->get_node_base_interface()->get_context())
              throw std::invalid_argument(
                  "GPIO topic pin belongs to another context/address");
            state->topic = topic;
            state->transport = GpioPublisher::SharedPtr(
                topic->entities, topic->entities->publisher.get());
            return address.topic ==
                       topic->entities->publisher->get_topic_name() &&
                   entities->node->get_node_topics_interface()
                           ->resolve_topic_name(address.topic) == address.topic;
          } else {
            bool exact = false;
            invoke([&] {
              entities->client = entities->node->create_client<Service>(
                  address.service, rclcpp::ServicesQoS(),
                  entities->callback_group);
              entities->InstallServiceObserver(owner);
              exact = address.service == entities->client->get_service_name();
              state->transport =
                  Client::SharedPtr(entities, entities->client.get());
            });
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
    state->cached_service_ready.store(entities->service_ready, std::memory_order_release);
    state->cached_context.store(entities->context, std::memory_order_release);
    return Reply::Ok(std::unique_ptr<RosBooleanOutputProvider>(
        new RosBooleanOutputProvider(std::move(state), std::move(entities))));
  } catch (const core::TypedError &error) {
    if (error.code() == "ros_transport_executor_failed" ||
        (error.code() == "component_not_ready" &&
         error.details().contains("reason") &&
         error.details().at("reason") == "owner_closing"))
      return Reply::Err(error.error());
    return Reply::Err({"io_ros_output_setup_failed", error.what(), {}});
  } catch (const std::exception &error) {
    return Reply::Err({"io_ros_output_setup_failed", error.what(), {}});
  }
}

RosBooleanOutputProvider::RosBooleanOutputProvider(
    std::shared_ptr<State> state, std::shared_ptr<Entities> entities)
    : state_(std::move(state)), entities_(std::move(entities)) {}
std::shared_ptr<RosBooleanOutputProvider::GpioTopic>
RosBooleanOutputProvider::gpio_topic() const noexcept {
  return state_->topic;
}

RosBooleanOutputProvider::~RosBooleanOutputProvider() { Shutdown(); }

SetOutputResult
RosBooleanOutputProvider::WriteOutput(const core::IoSignalId &signal,
                                      const bool value,
                                      const std::stop_token stop) {
  std::shared_ptr<const void> driver_pin;
  std::vector<std::shared_ptr<State>> channel_pins;
  std::vector<std::shared_ptr<const void>> writer_pins;
  Client::SharedPtr client_borrow;
  std::shared_ptr<Entities> service_entities;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> service_owner;
  std::shared_ptr<ros2_control_execution::GraphEventObserver> observer_borrow;
  Wire wire;
  std::shared_ptr<GpioTopic::TopicEntities> gpio_entities;
  bool gpio_selected = false;
  std::shared_ptr<const State::ModelFacts> own_facts;
  std::optional<State::Gate> selected_gate;
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
    if (!state_->Gpio()) {
      service_owner = state_->owner.lock();
      if (service_owner && service_owner->on_owner_thread())
        return write_failed("io_output_not_submitted",
                            "the output cannot wait on its ROS owner; nothing was sent");
      client_borrow = std::get<Client::SharedPtr>(state_->transport);
      service_entities = entities_;
      own_facts = state_->model_facts.load(std::memory_order_acquire);
      selected_gate = gate;
      observer_borrow = entities_->service_observer;
      auto request = std::make_shared<Service::Request>();
      request->data = value;
      wire = std::move(request);
    } else {
      gpio_selected = true;
      gpio_entities = state_->topic->entities;
      own_facts = state_->model_facts.load(std::memory_order_acquire);
      selected_gate = gate;
    }
  }
  if (gpio_selected) {
    if (!gpio_entities || !gpio_entities->publisher ||
        !gpio_entities->node->get_node_base_interface()->get_context()->is_valid())
      return write_failed("io_output_not_submitted",
                          "the output's service is not available; nothing was sent");
    const auto gpio_owner = gpio_entities->owner.lock();
    if (gpio_entities->owner_required && !gpio_owner)
      return write_failed("io_output_not_submitted", "the output owner is unavailable; nothing was sent");
    std::optional<State::Written> committed_model;
    bool sent = false;
    bool prepared = false;
    {
      MANYFORGE_LEAF_LOCK(leaf, gpio_entities->send_mutex,
          ros2_control_execution::detail::LeafEntity::kGpioPublish);
      try {
        const auto current = state_->model_facts.load(std::memory_order_acquire);
        const bool admitted = gpio_entities->accepting.load(std::memory_order_acquire) &&
            (!gpio_owner || !gpio_owner->executor_failure_started()) &&
            current && !current->retired && current->publication == own_facts->publication &&
            std::any_of(current->gates.begin(), current->gates.end(), [&](const auto &gate) {
              return gate.accepting && !gate.terminal &&
                     gate.generation == selected_gate->generation &&
                     gate.provider == selected_gate->provider &&
                     SameIoOutputDriverOwner(gate.actual_driver_owner,
                                             selected_gate->actual_driver_owner);
            });
        if (!admitted) throw std::invalid_argument("GPIO admission closed");
        MANYFORGE_GPIO_ADMITTED_TEST_POINT();
        wire = State::BuildWire(state_, value, *selected_gate, gpio_entities, own_facts,
                                channel_pins, writer_pins, committed_model);
        gpio_entities->written_model.try_emplace(
            state_->resource, GpioTopic::Model{state_, std::nullopt});
        prepared = true;
        gpio_entities->publisher->publish(std::get<GpioMessage>(wire));
        sent = true;
      } catch (const std::exception &) {}
      if (prepared) {
        if (sent)
          gpio_entities->written_model.at(state_->resource) = {state_, std::move(committed_model)};
        else
          gpio_entities->written_model.erase(state_->resource);
      }
    }
    if (!prepared)
      return write_failed("io_output_not_submitted",
                          "the output write could not be prepared; nothing was sent");
    if (!sent)
      return write_failed("io_output_acceptance_unknown", "the output publish's outcome is unknown",
                          core::ActionOutcome::kUnknown);
    return {core::ActionOutcome::kSucceeded, std::nullopt};
  }
  // Check the exact captured client without holding the channel or topic lock.
  if (!client_borrow || !client_borrow->service_is_ready())
    return write_failed("io_output_not_submitted",
                        "the output's service is not available; nothing was sent");
  // One request, its answer awaited outside the channel lock.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::nanoseconds(state_->ack_timeout_ns);
  const auto await = [&](const auto &client, const auto &request) -> SetOutputResult {
    const auto &owner = service_owner;
    if (service_entities->owner_required && !owner)
      return write_failed("io_output_not_submitted", "the output owner is unavailable; nothing was sent");
    using Pending = decltype(client->async_send_request(request));
    std::optional<Pending> submitted;
    {
#ifdef MANYFORGE_LEAF_TIMING
      ros2_control_execution::detail::io_service_waiting.store(true, std::memory_order_release);
#endif
      MANYFORGE_LEAF_LOCK(leaf, service_entities->send_mutex,
          ros2_control_execution::detail::LeafEntity::kIoServiceSend);
      const auto current = state_->model_facts.load(std::memory_order_acquire);
      const bool admitted = service_entities->accepting.load(std::memory_order_acquire) &&
          (!owner || !owner->executor_failure_started()) && current && !current->retired &&
          current->publication == own_facts->publication &&
          std::any_of(current->gates.begin(), current->gates.end(), [&](const auto &gate) {
            return gate.accepting && !gate.terminal &&
                   gate.generation == selected_gate->generation &&
                   gate.provider == selected_gate->provider &&
                   SameIoOutputDriverOwner(gate.actual_driver_owner,
                                           selected_gate->actual_driver_owner);
          });
      if (!admitted)
        return write_failed("io_output_not_submitted", "the output admission is closed; nothing was sent");
      if (std::chrono::steady_clock::now() >= deadline)
        return write_failed("io_output_not_submitted", "the output deadline expired; nothing was sent");
      MANYFORGE_IO_SERVICE_ADMITTED_TEST_POINT();
      submitted.emplace(client->async_send_request(request));
    }
    auto &future = *submitted;
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
    auto result = await(client_borrow,
                        std::get<std::shared_ptr<Service::Request>>(wire));
    if (observer_borrow) (void)observer_borrow->request_refresh();
    return result;
  } catch (const std::exception &) {
    if (observer_borrow) (void)observer_borrow->request_refresh();
    // A send that threw may still have left: unknown, never resent.
    return write_failed("io_output_acceptance_unknown",
                        "the output write's outcome is unknown", core::ActionOutcome::kUnknown);
  }
}

std::optional<bool> RosBooleanOutputProvider::CachedServiceReady() const noexcept {
  const auto context = state_->cached_context.load(std::memory_order_acquire);
  if (!context || !context->valid()) return false;
  if (state_->Gpio()) return true;
  const auto sample = state_->cached_service_ready.load(std::memory_order_acquire);
  if (!sample) return std::nullopt;
  const int value = sample->load(std::memory_order_acquire);
  if (value < 0) return std::nullopt;
  return value > 0;
}

bool RosBooleanOutputProvider::ServiceReady() const {
  Client::SharedPtr client;
  std::shared_ptr<GpioTopic::TopicEntities> gpio;
  {
    std::lock_guard lock(state_->mutex);
    auto topic_lock = state_->LockTopic();
    if (state_->retired) return false;
    if (state_->Gpio()) gpio = state_->topic->entities;
    else client = std::get<Client::SharedPtr>(state_->transport);
  }
  try {
    if (gpio)
      return gpio->publisher && gpio->node->get_node_base_interface()->get_context()->is_valid();
    return client && client->service_is_ready();
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
  state_->RefreshModelFacts();
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
  state_->RefreshModelFacts();
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
  state_->RefreshModelFacts();
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
  state_->RefreshModelFacts();
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
  entities_->accepting.store(false, std::memory_order_release);
  state_->cached_service_ready.store({}, std::memory_order_release);
  state_->cached_context.store({}, std::memory_order_release);
  for (auto &gate : state_->gates)
    gate.accepting = false;
  state_->model_facts.store({}, std::memory_order_release);
}

} // namespace manyforge::roboplan_backend
