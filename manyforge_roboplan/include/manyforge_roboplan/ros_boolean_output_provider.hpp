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

#pragma once

#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_roboplan/io_output_alias_identity.hpp>
#include <manyforge_roboplan/io_output_writer.hpp>
#include <memory>
#include <rclcpp/node.hpp>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {

struct RosBooleanOutputAlias final {
  core::IoSignalId signal_id;
  core::ResolvedLifecycleProviderOccurrenceDTO provider;
  std::uint64_t prepared_driver_generation;
  std::weak_ptr<const void> actual_driver_owner;
};

struct RosSetBoolOutputAddress final {
  std::string service;
};
struct RosGpioOutputAddress final {
  std::string topic;
  std::string group;
  std::string interface_name;
};
using RosBooleanOutputAddress =
    std::variant<RosSetBoolOutputAddress, RosGpioOutputAddress>;

struct RosBooleanOutputChannel final {
  core::ResourceId resource_id;
  RosBooleanOutputAddress address;
  std::vector<RosBooleanOutputAlias> aliases;
  core::ActiveBundlePublicationGeneration active_publication_generation;
  /// A SetBool service's answer bound: the device's authored
  /// `output_source_ack_timeout_s` (CONTRACTS §7). Unused for GPIO, which
  /// has no answer.
  core::Duration ack_timeout_ns{};
};

/// One resolved SetBool service or GPIO command topic on the existing host ROS
/// context. Composition groups every channel alias into one provider;
/// per-occurrence driver gates remain separate. There is no owned node,
/// executor, thread, timer, resend, input publication or readback. A true
/// service response is an accepted write, never evidence that hardware changed
/// state; a GPIO publish has no acknowledgement or readback. Shutdown and
/// quiesce the existing executor before releasing this provider.
class RosBooleanOutputProvider final {
public:
  /// Opaque shared transport/write-model of one actual ROS context+command
  /// topic. It owns no lifecycle/admission or command correlation authority.
  struct GpioTopic;
  [[nodiscard]] static core::Result<std::unique_ptr<RosBooleanOutputProvider>>
  Create(rclcpp::Node::SharedPtr node, RosBooleanOutputChannel channel,
         std::shared_ptr<GpioTopic> gpio_topic = {});
  /// Actual retained topic pin only; null for acknowledged service outputs.
  [[nodiscard]] std::shared_ptr<GpioTopic> gpio_topic() const noexcept;
  ~RosBooleanOutputProvider();
  RosBooleanOutputProvider(const RosBooleanOutputProvider &) = delete;
  RosBooleanOutputProvider &
  operator=(const RosBooleanOutputProvider &) = delete;

  /// CONTRACTS §6, §7: the one write of one alias, behind its component's
  /// output-writer interface: one send while the alias's occurrence is
  /// enabled. A service
  /// answer is the outcome; no answer within the channel's `ack_timeout_ns`,
  /// or a Stop, is `unknown` (never resent). A GPIO publish has no answer: published is
  /// succeeded.
  [[nodiscard]] SetOutputResult WriteOutput(const core::IoSignalId &signal, bool value,
                                            std::stop_token stop);

  /// Actual client connectivity / publisher resource only; no waiting,
  /// subscriber count guarantee or lifecycle-state inference.
  [[nodiscard]] bool ServiceReady() const;
  /// Existing host delivery/domain owner supplies its actual observation
  /// generation after full lifecycle checks. No CCLC generation is minted.
  core::Result<void>
  Pause(const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
        std::uint64_t driver_observation_generation,
        const std::weak_ptr<const void> &actual_driver_owner);
  core::Result<void>
  Resume(const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
         std::uint64_t driver_observation_generation,
         const std::weak_ptr<const void> &actual_driver_owner);
  /// Final local owner teardown, independent of counter exhaustion. It fences
  /// only this exact occurrence.
  core::Result<void> ShutdownOccurrence(
      const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
      const std::weak_ptr<const void> &actual_driver_owner);
  /// Current canonical table for the trusted installer to join to its exact
  /// sealed predecessor/original candidate and held parent Apply proof.
  [[nodiscard]] IoOutputAliasState ReadAliasState() const;
  /// Trusted installer only, under the existing exact Apply pin. Atomically
  /// replaces the complete alias table, preserving current retained gates and
  /// pending correlation. New actual owners start paused. Exact publication
  /// replay is a no-op; predecessor comparison rejects stale metadata.
  core::Result<void> RebindAliases(
      const std::vector<IoOutputAliasIdentity> &expected_current_aliases,
      const std::vector<RosBooleanOutputAlias> &candidate_aliases,
      core::ActiveBundlePublicationGeneration active_publication_generation);
  void Shutdown();

private:
  struct State;
  struct Entities;
  RosBooleanOutputProvider(std::shared_ptr<State> state,
                           std::unique_ptr<Entities> entities);
  std::shared_ptr<State> state_;
  std::unique_ptr<Entities> entities_;
};

} // namespace manyforge::roboplan_backend
