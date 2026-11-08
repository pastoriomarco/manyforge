#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include "io_provider_lifecycle_product.hpp"
#include "cached_ros_context.hpp"
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/ros_mapped_boolean_input_provider.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <variant>
#include <type_traits>

namespace manyforge::roboplan_backend::host_detail {
namespace {
using CachedInputContext = CachedRosContext;

core::ErrorDTO error(std::string reason) {
  return {"io_provider_lifecycle_unavailable",
          "The actual I/O driver cannot satisfy this request",
          {{"reason", std::move(reason)}}};
}

bool is_ros_owner_refusal(const core::ErrorDTO &error) {
  return error.code == "ros_transport_executor_failed" ||
         (error.code == "component_not_ready" &&
          error.details.contains("reason") &&
          error.details.at("reason") == "owner_closing");
}

bool same_rows(
    const std::vector<deployment::RetainedIoSignalConfiguration> &a,
    const std::vector<deployment::RetainedIoSignalConfiguration> &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tie(a[i].runtime_unit_id, a[i].command_target_id,
                 a[i].resource_role_id, a[i].descriptor, a[i].provider) !=
        std::tie(b[i].runtime_unit_id, b[i].command_target_id,
                 b[i].resource_role_id, b[i].descriptor, b[i].provider))
      return false;
  return true;
}

core::ActionResult action_result(core::ActionOutcome outcome,
                                 std::optional<core::ErrorDTO> error = std::nullopt) {
  core::ActionResult result;
  result.outcome = outcome;
  if (error)
    result.error = core::ActionError{std::move(error->code), std::move(error->message),
                                     std::nullopt, std::nullopt};
  return result;
}

core::ErrorDTO senders_gone() {
  return {"io_device_senders_gone",
          "Every sender of this I/O device's inputs has gone", {}};
}

core::ComponentStream stream_of(const core::StreamHealthDTO &health) {
  return {health.stream_id,
          health.state == core::StreamLivenessStateDTO::kLive    ? core::StreamFreshness::kFresh
          : health.state == core::StreamLivenessStateDTO::kStale ? core::StreamFreshness::kStale
                                                                  : core::StreamFreshness::kNone,
          health.age_ns};
}
} // namespace

struct IoProviderDriverRecord::Driver final {
  Driver(core::ResolvedLifecycleControlDomainScopeDTO selected_domain,
        core::ResolvedLifecycleProviderOccurrenceDTO selected_occurrence,
        core::ProviderManifestDTO selected_manifest,
        std::shared_ptr<core::IClock> selected_clock)
      : domain(std::move(selected_domain)),
        occurrence(std::move(selected_occurrence)),
        manifest(std::move(selected_manifest)), clock(std::move(selected_clock)) {
  }
  struct Input final {
    IoProviderInputSourceInstallation installation;
    std::variant<std::shared_ptr<MockBooleanInputProvider>,
                 std::shared_ptr<RosBooleanInputProvider>,
                 std::shared_ptr<RosMappedBooleanInputProvider>,
                 std::shared_ptr<GeometricContactInputProvider>>
        leaf;
    std::shared_ptr<CachedInputContext> context;
  };
  core::ResolvedLifecycleControlDomainScopeDTO domain;
  core::ResolvedLifecycleProviderOccurrenceDTO occurrence;
  core::ProviderManifestDTO manifest;
  std::shared_ptr<core::IClock> clock;
  std::atomic<std::shared_ptr<ros2_control_execution::Ros2TransportResources>>
      ros_owner;
  std::atomic<bool> observer_installed{false};
  mutable std::mutex mutex;
  std::vector<Input> inputs;
  std::vector<IoProviderOutputChannelInstallation> outputs;
  std::function<void()> cancel_pending;
  InputLifecycleHook input_lifecycle;
  InputAcquisitionHook input_acquisition;
  core::ComponentLifecycleStateDTO state{
      core::ComponentLifecycleStateDTO::kDeclared};
  std::uint64_t generation{1U};
  bool installed{};
  bool enabled{};
  bool output_admission_open{true};
  bool ros_owner_recovery_closed{};
  bool retired{};
  std::optional<core::ErrorDTO> fault;
  std::weak_ptr<ros2_control_execution::Ros2TransportResources> fault_owner;
  void RecordFault(
      const core::ErrorDTO &cause,
      const std::shared_ptr<ros2_control_execution::Ros2TransportResources>
          &owner = {}) {
    fault = cause;
    fault_owner = owner;
  }
  void ClearFault() {
    fault.reset();
    fault_owner.reset();
  }
  std::function<std::optional<core::ErrorDTO>()> process_fault_source;
  std::vector<std::unique_ptr<core::StreamRecorder>> observer_streams;
  std::optional<std::vector<IoProviderInputSourceInstallation>> last_rebind;
  std::optional<core::ErrorDTO> last_rebind_error;
  /// IO-1: a ROS input topic had a publisher since install.
  bool senders_seen{};
  /// IO-1: every sender left while enabled; held until Recover or Disable.
  bool senders_lost{};
  /// IO-1: the optional `inputs` stream, shared with the ROS input leaves.
  std::shared_ptr<core::StreamRecorder> inputs_stream;

  // IO-1: the ROS input leaves (Mock and geometric inputs have no sender).
  template <typename Visit> bool ForRosInputs(Visit &&visit) {
    bool any = false;
    for (auto &input : inputs)
      std::visit(
          [&](auto &leaf) {
            using Leaf = std::decay_t<decltype(leaf)>;
            if constexpr (std::is_same_v<Leaf, std::shared_ptr<RosBooleanInputProvider>> ||
                          std::is_same_v<Leaf, std::shared_ptr<RosMappedBooleanInputProvider>>) {
              any = true;
              visit(input, *leaf);
            }
          },
          input.leaf);
    return any;
  }

  // IO-1: every ROS input topic has no matched compatible publisher, after one
  // had one since install. The count of matched compatible publishers per
  // leaf (ROS matched-publisher event), so a leaf's resubscription (Enable)
  // doesn't look like a lost sender.
  bool SendersGone() {
    bool present = false;
    bool unknown = false;
    const bool ros = ForRosInputs([&](const Input &, auto &leaf) {
      const auto count = leaf.matched_publishers();
      unknown = unknown || !count.has_value();
      present = present || (count && *count > 0U);
    });
    senders_seen = senders_seen || present;
    return ros && senders_seen && !present && !unknown;
  }

  // The supervised process's exit, never latched into `fault`: the
  // restarted process's source replaces it at the next Apply.
  std::optional<core::ErrorDTO> ProcessFault() const {
    if (retired || !process_fault_source)
      return std::nullopt;
    auto fault = process_fault_source();
    if (!fault)
      return std::nullopt;
    auto &reason = fault->message;
    if (reason.size() > core::kMaxLifecycleErrorMessageBytes) {
      auto end = core::kMaxLifecycleErrorMessageBytes;
      while (end > 0U &&
             (static_cast<unsigned char>(reason[end]) & 0xC0U) == 0x80U)
        --end;
      reason.resize(end);
    }
    return fault;
  }

  bool TransportReady() const {
    if (!installed || retired)
      return false;
    for (const auto &input : inputs)
      if (input.installation.node &&
          (!input.context || !input.context->valid()))
        return false;
    for (const auto &output : outputs) {
      const bool ready = std::visit(
          [](const auto &provider) {
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(provider)>,
                              std::shared_ptr<RosBooleanOutputProvider>>) {
              if (!provider) return false;
              return provider->CachedServiceReady().value_or(false);
            } else {
              return static_cast<bool>(provider);
            }
          },
          output.provider);
      if (!ready)
        return false;
    }
    return true;
  }

  // Presentation only: names the first configured transport that keeps
  // TransportReady() false. It never enters facts, snapshots or decisions.
  std::optional<core::ErrorDTO> TransportDiagnostic() const {
    if (!installed || retired)
      return std::nullopt;
    const auto missing =
        [](const std::vector<deployment::RetainedIoSignalConfiguration> &rows,
           const char *kind, const std::optional<std::string> &name)
        -> std::optional<core::ErrorDTO> {
      // State only what TransportReady() checked for this transport.
      const bool input = std::string_view(kind) == "input_topic";
      core::ErrorDTO diagnostic{
          "io_provider_transport_unavailable",
          input ? "The ROS context for input topic " + name.value_or("?") +
                      " is not valid"
          : name ? "ROS " + std::string(kind) + " " + *name +
                       " is not available"
                 : "The configured I/O output is not available",
          {{"transport_kind", kind}}};
      if (name)
        diagnostic.details.emplace("ros_name", *name);
      if (!rows.empty())
        diagnostic.details.emplace("signal_name",
                                   rows.front().descriptor.signal_id.name);
      return diagnostic;
    };
    for (const auto &input : inputs)
      if (input.installation.node &&
          (!input.context || !input.context->valid()))
        return missing(input.installation.rows, "input_topic",
                       input.installation.rows.empty()
                           ? std::nullopt
                           : input.installation.rows.front().provider.topic);
    for (const auto &output : outputs) {
      const auto *ros =
          std::get_if<std::shared_ptr<RosBooleanOutputProvider>>(
              &output.provider);
      const bool ready = [&] {
        if (!ros) return static_cast<bool>(std::get<0>(output.provider));
        if (!*ros) return false;
        return (*ros)->CachedServiceReady().value_or(false);
      }();
      if (ready)
        continue;
      const auto *row = output.rows.empty() ? nullptr : &output.rows.front();
      const bool service = row && row->provider.service;
      return missing(output.rows, service ? "service" : "topic",
                     !row       ? std::nullopt
                     : service ? row->provider.service
                               : row->provider.topic);
    }
    return std::nullopt;
  }

  bool Advance() {
    if (generation == std::numeric_limits<std::uint64_t>::max())
      return false;
    ++generation;
    return true;
  }


  bool Owns(const deployment::RetainedIoSignalConfiguration &row) const {
    return row.runtime_unit_id == occurrence.provider_runtime_unit_id &&
           row.descriptor.signal_id.controllable_component_id ==
               domain.provider()->provider.controllable_component_id;
  }

  bool
  SameRebind(const std::vector<IoProviderInputSourceInstallation> &a,
             const std::vector<IoProviderInputSourceInstallation> &b) const {
    if (a.size() != b.size())
      return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (!same_rows(a[i].rows, b[i].rows) || a[i].node != b[i].node ||
          a[i].ingress != b[i].ingress || a[i].incarnation != b[i].incarnation)
        return false;
    return true;
  }
};

IoProviderDriverRecord::IoProviderDriverRecord(std::unique_ptr<Driver> state)
    : state_(std::move(state)) {}
IoProviderDriverRecord::~IoProviderDriverRecord() { Shutdown(); }

core::Result<std::shared_ptr<IoProviderDriverRecord>>
IoProviderDriverRecord::Create(
    core::ResolvedLifecycleControlDomainScopeDTO domain,
    core::ResolvedLifecycleProviderOccurrenceDTO occurrence,
    core::ProviderManifestDTO manifest, std::shared_ptr<core::IClock> clock) {
  using Reply = core::Result<std::shared_ptr<IoProviderDriverRecord>>;
  const auto *provider = domain.provider();
  if (!domain.locally_valid() || !provider || !clock ||
      !core::ProviderDomainMatchesOccurrence(
          domain.control_domain_id, domain.control_domain_revision,
          provider->provider, provider->provider_runtime_epoch, occurrence))
    return Reply::Err(error("driver_occurrence_invalid"));
  if (manifest.provider_manifest_id != occurrence.provider_manifest_id ||
      manifest.provider_manifest_revision !=
          occurrence.provider_manifest_revision)
    return Reply::Err(error("driver_manifest_not_exact"));
  auto state = std::make_unique<Driver>(std::move(domain), std::move(occurrence),
                                       std::move(manifest), std::move(clock));
  return Reply::Ok(std::shared_ptr<IoProviderDriverRecord>(
      new IoProviderDriverRecord(std::move(state))));
}

const core::ResolvedLifecycleControlDomainScopeDTO &
IoProviderDriverRecord::domain() const noexcept {
  return state_->domain;
}
const core::ResolvedLifecycleProviderOccurrenceDTO &
IoProviderDriverRecord::occurrence() const noexcept {
  return state_->occurrence;
}
const core::ProviderManifestDTO &
IoProviderDriverRecord::manifest() const noexcept {
  return state_->manifest;
}
core::IClock &IoProviderDriverRecord::clock() const noexcept {
  return *state_->clock;
}
core::RuntimeUnitEpochBindingDTO
IoProviderDriverRecord::runtime_unit_epoch() const {
  return {state_->occurrence.provider_runtime_unit_id,
          state_->occurrence.provider_runtime_epoch};
}
bool IoProviderDriverRecord::resources_installed() const {
  auto lock = AcquireDelivery();
  return state_->installed && !state_->retired;
}
std::shared_ptr<ros2_control_execution::Ros2TransportResources>
IoProviderDriverRecord::RosOwner() const noexcept {
  return state_->ros_owner.load(std::memory_order_acquire);
}
std::unique_lock<std::mutex> IoProviderDriverRecord::AcquireDelivery() const {
  return std::unique_lock(state_->mutex);
}
std::uint64_t
IoProviderDriverRecord::driver_observation_generation_locked() const noexcept {
  return state_->generation;
}
struct IoProviderDriverRecord::RosOwnerRecoverySnapshot final {
  using Bool = RosBooleanInputProvider::TransportRecoverySnapshot;
  using Mapped = RosMappedBooleanInputProvider::TransportRecoverySnapshot;
  struct Input final {
    std::size_t index;
    std::variant<std::shared_ptr<const Bool>, std::shared_ptr<const Mapped>>
        leaf;
    rclcpp::Node::SharedPtr old_node;
    std::shared_ptr<CachedInputContext> old_context;
  };
  std::shared_ptr<IoProviderDriverRecord> driver;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner;
  std::uint64_t generation{};
  std::vector<Input> inputs;
};
struct IoProviderDriverRecord::RosOwnerRecoveryStage final {
  using Bool = RosBooleanInputProvider::TransportRecoveryStage;
  using Mapped = RosMappedBooleanInputProvider::TransportRecoveryStage;
  std::shared_ptr<RosOwnerRecoverySnapshot> snapshot;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner;
  rclcpp::Node::SharedPtr node;
  std::shared_ptr<CachedInputContext> context;
  std::vector<std::variant<std::shared_ptr<Bool>, std::shared_ptr<Mapped>>>
      inputs;
};

bool IoProviderDriverRecord::UsesRosOwnerLocked(
    const std::shared_ptr<ros2_control_execution::Ros2TransportResources>
        &owner) const {
  if (!owner || state_->retired || !state_->installed)
    return false;
  for (const auto &input : state_->inputs)
    if (input.installation.node == owner->node() &&
        (std::holds_alternative<std::shared_ptr<RosBooleanInputProvider>>(
             input.leaf) ||
         std::holds_alternative<std::shared_ptr<RosMappedBooleanInputProvider>>(
             input.leaf)))
      return true;
  for (const auto &output : state_->outputs)
    if (const auto *leaf =
            std::get_if<std::shared_ptr<RosBooleanOutputProvider>>(
                &output.provider))
      if (*leaf && (*leaf)->BelongsToTransport(owner))
        return true;
  return false;
}
core::Result<std::shared_ptr<IoProviderDriverRecord::RosOwnerRecoverySnapshot>>
IoProviderDriverRecord::CloseAndCaptureRosOwnerRecovery(
    const std::shared_ptr<ros2_control_execution::Ros2TransportResources>
        &owner,
    bool bridge_observer) {
  using Reply = core::Result<std::shared_ptr<RosOwnerRecoverySnapshot>>;
  auto lock = AcquireDelivery();
  if (!UsesRosOwnerLocked(owner) && !(bridge_observer && stream_observer() &&
                                      state_->installed && !state_->retired))
    return Reply::Err(error("driver_ros_owner_not_installed"));
  state_->ros_owner_recovery_closed = true;
  state_->output_admission_open = false;
  if (state_->generation == std::numeric_limits<std::uint64_t>::max())
    return Reply::Err(error("driver_generation_exhausted"));
  const auto inputs = state_->inputs;
  auto outputs = state_->outputs;
  const auto lifecycle = state_->input_lifecycle;
  const auto cancel_pending = state_->cancel_pending;
  const auto occurrence = state_->occurrence;
  const auto initial_generation = state_->generation;
  const auto same_inputs = [&] {
    if (state_->retired || !state_->installed || state_->inputs.size() != inputs.size() ||
        state_->outputs.size() != outputs.size()) return false;
    for (std::size_t i = 0; i < inputs.size(); ++i)
      if (state_->inputs[i].leaf != inputs[i].leaf) return false;
    for (std::size_t i = 0; i < outputs.size(); ++i)
      if (state_->outputs[i].provider != outputs[i].provider) return false;
    return true;
  };
  std::optional<core::ErrorDTO> failure;
  lock.unlock();
  if (!inputs.empty() && lifecycle) {
    const auto planned = lifecycle(true);
    if (!planned.ok())
      failure = planned.error();
  }
  lock.lock();
  if (!same_inputs() || state_->generation != initial_generation) {
    lock.unlock();
    return Reply::Err(error("driver_rebind_membership_changed"));
  }
  if (!state_->Advance())
    return Reply::Err(error("driver_generation_exhausted"));
  const auto generation = state_->generation;
  lock.unlock();
  try {
    if (cancel_pending)
      cancel_pending();
  } catch (const std::exception &) {
    failure = error("driver_timer_cancellation_failed");
  }
  for (const auto &input : inputs)
    if (!std::visit([&](const auto &leaf) { return leaf->Pause(generation); },
                    input.leaf))
      failure = error("driver_input_gate_failed");
  for (auto &output : outputs) {
    const auto paused = std::visit(
        [&](auto &provider) {
          return provider->Pause(occurrence, generation,
                                 weak_from_this());
        },
        output.provider);
    if (!paused.ok())
      failure = paused.error();
  }
  lock.lock();
  if (!same_inputs() || state_->generation != generation) {
    lock.unlock();
    return Reply::Err(error("driver_rebind_membership_changed"));
  }
  state_->enabled = false;
  if (failure)
    return Reply::Err(*failure);
  auto snapshot = std::make_shared<RosOwnerRecoverySnapshot>();
  snapshot->driver = shared_from_this();
  snapshot->owner = state_->ros_owner.load(std::memory_order_acquire);
  snapshot->generation = state_->generation;
  for (std::size_t i = 0; i < state_->inputs.size(); ++i) {
    auto &input = state_->inputs[i];
    if (input.installation.node != owner->node())
      continue;
    if (auto *leaf =
            std::get_if<std::shared_ptr<RosBooleanInputProvider>>(&input.leaf))
      snapshot->inputs.push_back(
          {i, (*leaf)->CaptureTransportRecovery(), input.installation.node, input.context});
    else if (auto *leaf =
                 std::get_if<std::shared_ptr<RosMappedBooleanInputProvider>>(
                     &input.leaf))
      snapshot->inputs.push_back(
          {i, (*leaf)->CaptureTransportRecovery(), input.installation.node, input.context});
  }
  return Reply::Ok(std::move(snapshot));
}
core::Result<std::shared_ptr<IoProviderDriverRecord::RosOwnerRecoveryStage>>
IoProviderDriverRecord::StageRosOwnerRecovery(
    std::shared_ptr<RosOwnerRecoverySnapshot> snapshot,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner) {
  using Reply = core::Result<std::shared_ptr<RosOwnerRecoveryStage>>;
  if (!snapshot || !owner)
    return Reply::Err(error("driver_ros_recovery_invalid"));
  auto stage = std::make_shared<RosOwnerRecoveryStage>();
  stage->snapshot = std::move(snapshot);
  stage->owner = owner;
  stage->node = owner->retain_entity(owner->node());
  stage->context = std::make_shared<CachedInputContext>(
      stage->node->get_node_base_interface()->get_context());
  for (const auto &input : stage->snapshot->inputs) {
    if (const auto *leaf = std::get_if<std::shared_ptr<
            const RosBooleanInputProvider::TransportRecoverySnapshot>>(
            &input.leaf)) {
      auto prepared =
          RosBooleanInputProvider::StageTransportRecovery(*leaf, owner);
      if (!prepared.ok())
        return Reply::Err(prepared.error());
      stage->inputs.emplace_back(std::move(prepared.value()));
    } else {
      auto prepared = RosMappedBooleanInputProvider::StageTransportRecovery(
          std::get<std::shared_ptr<
              const RosMappedBooleanInputProvider::TransportRecoverySnapshot>>(
              input.leaf),
          owner);
      if (!prepared.ok())
        return Reply::Err(prepared.error());
      stage->inputs.emplace_back(std::move(prepared.value()));
    }
  }
  return Reply::Ok(std::move(stage));
}
bool IoProviderDriverRecord::MatchesRosOwnerRecoveryLocked(
    const RosOwnerRecoverySnapshot &snapshot) const noexcept {
  if (snapshot.driver.get() != this || state_->retired || !state_->installed ||
      state_->generation != snapshot.generation || state_->enabled ||
      state_->ros_owner.load(std::memory_order_acquire) != snapshot.owner)
    return false;
  for (const auto &input : snapshot.inputs) {
    if (input.index >= state_->inputs.size())
      return false;
    const auto &current = state_->inputs[input.index];
    if (current.installation.node != input.old_node || current.context != input.old_context)
      return false;
    if (const auto *snap = std::get_if<std::shared_ptr<
            const RosBooleanInputProvider::TransportRecoverySnapshot>>(
            &input.leaf)) {
      const auto *leaf =
          std::get_if<std::shared_ptr<RosBooleanInputProvider>>(&current.leaf);
      if (!leaf || !(*leaf)->MatchesTransportRecovery(**snap))
        return false;
    } else {
      const auto *leaf =
          std::get_if<std::shared_ptr<RosMappedBooleanInputProvider>>(
              &current.leaf);
      if (!leaf ||
          !(*leaf)->MatchesTransportRecovery(
              *std::get<std::shared_ptr<const RosMappedBooleanInputProvider::
                                            TransportRecoverySnapshot>>(
                  input.leaf)))
        return false;
    }
  }
  return true;
}
void IoProviderDriverRecord::CommitRosOwnerRecoveryLocked(
    RosOwnerRecoveryStage &stage) noexcept {
  for (std::size_t i = 0; i < stage.inputs.size(); ++i) {
    auto &input = state_->inputs[stage.snapshot->inputs[i].index];
    if (auto *leaf = std::get_if<
            std::shared_ptr<RosBooleanInputProvider::TransportRecoveryStage>>(
            &stage.inputs[i]))
      std::get<std::shared_ptr<RosBooleanInputProvider>>(input.leaf)
          ->CommitTransportRecovery(**leaf);
    else
      std::get<std::shared_ptr<RosMappedBooleanInputProvider>>(input.leaf)
          ->CommitTransportRecovery(
              *std::get<std::shared_ptr<
                  RosMappedBooleanInputProvider::TransportRecoveryStage>>(
                  stage.inputs[i]));
    input.installation.node = stage.node;
    input.context = stage.context;
  }
  state_->state = core::ComponentLifecycleStateDTO::kPrepared;
  if (stage.snapshot->owner &&
      state_->fault_owner.lock() == stage.snapshot->owner)
    state_->ClearFault();
  if (stage.snapshot->owner)
    state_->ros_owner.store(stage.owner, std::memory_order_release);
  state_->ros_owner_recovery_closed = false;
}
void IoProviderDriverRecord::ActivateRosOwnerRecovery(
    RosOwnerRecoveryStage &stage) noexcept {
  for (auto &input : stage.inputs)
    if (auto *leaf = std::get_if<
            std::shared_ptr<RosBooleanInputProvider::TransportRecoveryStage>>(
            &input))
      RosBooleanInputProvider::ActivateTransportRecovery(**leaf);
    else
      RosMappedBooleanInputProvider::ActivateTransportRecovery(
          *std::get<std::shared_ptr<
              RosMappedBooleanInputProvider::TransportRecoveryStage>>(input));
}

core::Result<void> IoProviderDriverRecord::InstallResourcesLocked(
    std::unique_lock<std::mutex> &delivery,
    std::vector<IoProviderInputSourceInstallation> inputs,
    std::vector<IoProviderOutputChannelInstallation> outputs,
    std::function<void()> cancel_pending_answers,
    InputLifecycleHook input_lifecycle, InputAcquisitionHook input_acquisition,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner) {
  using Reply = core::Result<void>;
  if (owner)
    for (auto &input : inputs)
      input.node = owner->retain_entity(std::move(input.node));
  if (state_->installed || state_->retired ||
      (inputs.empty() && outputs.empty()))
    return Reply::Err(error("driver_resources_already_installed_or_absent"));
  std::set<std::string> members;
  std::set<core::InterruptSourceId> sources;
  std::set<core::ResourceId> resources;
  for (const auto &input : inputs) {
    if (input.rows.empty() || !input.ingress || input.incarnation.empty() ||
        !sources.insert(input.rows.front().descriptor.source_id).second)
      return Reply::Err(error("driver_input_source_invalid"));
    const auto &first = input.rows.front();
    for (const auto &row : input.rows)
      if (!state_->Owns(row) ||
          row.descriptor.direction != core::IoSignalDirection::kInput ||
          row.descriptor.source_id != first.descriptor.source_id ||
          row.provider.profile != first.provider.profile ||
          row.provider.ingress_depth != first.provider.ingress_depth ||
          row.provider.topic != first.provider.topic ||
          !members.insert(row.descriptor.signal_id.name).second)
        return Reply::Err(error("driver_input_membership_invalid"));
  }
  for (const auto &output : outputs) {
    if (output.rows.empty() || !resources.insert(output.resource_id).second ||
        !std::visit([](const auto &p) { return static_cast<bool>(p); },
                    output.provider))
      return Reply::Err(error("driver_output_channel_invalid"));
    for (const auto &row : output.rows)
      if (!state_->Owns(row) ||
          row.descriptor.direction != core::IoSignalDirection::kOutput ||
          row.descriptor.output_resource != output.resource_id ||
          (row.provider.profile == "mock_output"
               ? !std::holds_alternative<
                     std::shared_ptr<MockBooleanOutputProvider>>(
                     output.provider)
           : (row.provider.profile == "ros_set_bool" ||
              row.provider.profile == "ros2_control_gpio")
               ? !std::holds_alternative<
                     std::shared_ptr<RosBooleanOutputProvider>>(output.provider)
               : true) ||
          !members.insert(row.descriptor.signal_id.name).second)
        return Reply::Err(error("driver_output_membership_invalid"));
  }
  if (std::vector<std::string>(members.begin(), members.end()) !=
          state_->domain.provider()->provider.signal_names ||
      std::vector<core::ResourceId>(resources.begin(), resources.end()) !=
          state_->domain.resource_ids())
    return Reply::Err(error("driver_resource_closure_not_exact"));
  bool uses_ros_owner = false;
  if (owner) {
    for (const auto &input : inputs) {
      const auto &profile = input.rows.front().provider.profile;
      if (profile == "ros_bool" || profile == "ros2_control_gpio") {
        if (input.node != owner->node())
          return Reply::Err(error("driver_ros_owner_mismatch"));
        uses_ros_owner = true;
      }
    }
    for (const auto &output : outputs)
      if (const auto *leaf =
              std::get_if<std::shared_ptr<RosBooleanOutputProvider>>(
                  &output.provider)) {
        if (!(*leaf)->BelongsToTransport(owner))
          return Reply::Err(error("driver_ros_owner_mismatch"));
        uses_ros_owner = true;
      }
  }
  if (state_->generation == std::numeric_limits<std::uint64_t>::max())
    return Reply::Err(error("driver_generation_exhausted"));
  const auto initial_generation = state_->generation;
  const auto clock = state_->clock;
  delivery.unlock();
  using StageReply = core::Result<std::vector<Driver::Input>>;
  auto staged = [&]() -> StageReply {
    std::vector<Driver::Input> prepared_inputs;
  prepared_inputs.reserve(inputs.size());
  for (auto &input : inputs) {
    auto input_context = input.node ? std::make_shared<CachedInputContext>(
        input.node->get_node_base_interface()->get_context()) : nullptr;
    const auto &first = input.rows.front();
    if (first.provider.profile == "mock") {
      std::vector<MockBooleanInput> signals;
      for (const auto &row : input.rows) {
        if (!row.provider.initial_value)
          return StageReply::Err(error("driver_mock_initial_value_absent"));
        signals.push_back({row.descriptor, *row.provider.initial_value});
      }
      auto leaf = MockBooleanInputProvider::Create(
          input.ingress, *clock,
          {first.descriptor.source_id, input.incarnation, std::move(signals)},
          initial_generation);
      if (!leaf.ok())
        return StageReply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value()), std::move(input_context)});
    } else if (first.provider.profile == "ros_bool") {
      if (!input.node || input.rows.size() != 1U || !first.provider.topic)
        return StageReply::Err(error("driver_ros_bool_group_unsupported"));
      auto leaf = RosBooleanInputProvider::Create(
          input.node, input.ingress, clock,
          {first.descriptor.signal_id, first.descriptor.source_id,
           input.incarnation, *first.provider.topic},
          first.descriptor,
          rclcpp::QoS(rclcpp::KeepLast(first.provider.ingress_depth)),
          initial_generation, owner);
      if (!leaf.ok())
        return StageReply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value()), std::move(input_context)});
    } else if (first.provider.profile == "geometric_contact") {
      if (!input.geometric_contact || !input.geometric_contact->scheduler)
        return StageReply::Err(error("driver_geometric_contact_binding_invalid"));
      auto binding = *input.geometric_contact;
      binding.incarnation = input.incarnation;
      binding.signals.clear();
      for (const auto &row : input.rows)
        binding.signals.push_back(row.descriptor);
      const auto scheduler = binding.scheduler;
      auto leaf = GeometricContactInputProvider::Create(
          input.ingress, clock, *scheduler, std::move(binding),
          initial_generation);
      if (!leaf.ok())
        return StageReply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value()), std::move(input_context)});
    } else if (first.provider.profile == "ros2_control_gpio") {
      if (!input.node || !first.provider.topic)
        return StageReply::Err(error("driver_ros_mapped_source_invalid"));
      std::vector<RosMappedBooleanInputSignal> signals;
      signals.reserve(input.rows.size());
      for (const auto &row : input.rows) {
        if (!row.provider.group || !row.provider.interface_name)
          return StageReply::Err(error("driver_gpio_input_selector_absent"));
        signals.push_back(
            {row.descriptor,
             RosGpioBooleanInputSelector{*row.provider.group,
                                         *row.provider.interface_name}});
      }
      auto leaf = RosMappedBooleanInputProvider::Create(
          input.node, input.ingress, clock,
          {first.descriptor.source_id, input.incarnation, *first.provider.topic,
           std::move(signals)},
          rclcpp::QoS(rclcpp::KeepLast(first.provider.ingress_depth)),
          initial_generation, owner);
      if (!leaf.ok())
        return StageReply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value()), std::move(input_context)});
    } else
      return StageReply::Err(error("driver_input_profile_unsupported"));
  }
    return StageReply::Ok(std::move(prepared_inputs));
  }();
  delivery.lock();
  if (!staged.ok()) {
    const auto failure = staged.error();
    delivery.unlock();
    inputs.clear();
    outputs.clear();
    cancel_pending_answers = {};
    input_lifecycle = {};
    input_acquisition = {};
    owner.reset();
    delivery.lock();
    return Reply::Err(failure);
  }
  if (state_->retired || state_->installed || state_->generation != initial_generation) {
    delivery.unlock();
    return Reply::Err(error("driver_rebind_membership_changed"));
  }
  auto prepared_inputs = std::move(staged.value());
  if (!state_->Advance())
    return Reply::Err(error("driver_generation_exhausted"));
  state_->inputs = std::move(prepared_inputs);
  state_->outputs = std::move(outputs);
  state_->cancel_pending = std::move(cancel_pending_answers);
  state_->input_lifecycle = std::move(input_lifecycle);
  state_->input_acquisition = std::move(input_acquisition);
  state_->installed = true;
  if (uses_ros_owner)
    state_->ros_owner.store(owner, std::memory_order_release);
  return Reply::Ok();
}

core::Result<MockBooleanInputSetReceipt>
IoProviderDriverRecord::SetMockValueLocked(const core::IoSignalId &signal,
                                           bool value) {
  for (auto &input : state_->inputs)
    if (const auto *mock =
            std::get_if<std::shared_ptr<MockBooleanInputProvider>>(&input.leaf))
      if (std::any_of(input.installation.rows.begin(),
                      input.installation.rows.end(), [&](const auto &row) {
                        return row.descriptor.signal_id == signal;
                      }))
        return (*mock)->SetValue(signal, value);
  return core::Result<MockBooleanInputSetReceipt>::Err(
      error("driver_mock_signal_not_owned"));
}

core::Result<void> IoProviderDriverRecord::RebindInputsLocked(
    std::unique_lock<std::mutex> &delivery,
    std::vector<IoProviderInputSourceInstallation> inputs,
    std::function<void()> cancel_pending_answers,
    InputLifecycleHook input_lifecycle,
    InputAcquisitionHook input_acquisition,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner) {
  using Reply = core::Result<void>;
  if (owner)
    for (auto &input : inputs)
      input.node = owner->retain_entity(std::move(input.node));
  if (state_->retired || !state_->installed || state_->fault ||
      inputs.size() != state_->inputs.size())
    return Reply::Err(error("driver_rebind_unavailable"));
  if (state_->last_rebind && state_->SameRebind(inputs, *state_->last_rebind)) {
    if (state_->last_rebind_error)
      return Reply::Err(*state_->last_rebind_error);
    return Reply::Ok();
  }
  bool changed = false;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const auto &current = state_->inputs[i].installation;
    if (!same_rows(inputs[i].rows, current.rows) ||
        inputs[i].node != current.node || !inputs[i].ingress ||
        inputs[i].incarnation.empty())
      return Reply::Err(error("driver_rebind_membership_changed"));
    changed = changed || inputs[i].incarnation != current.incarnation;
  }
  if (!changed)
    return Reply::Err(error("driver_rebind_has_no_new_apply_identity"));
  auto pinned = state_->inputs;
  std::vector<IoProviderInputSourceInstallation> retired_installations;
  retired_installations.reserve(pinned.size());
  const auto initial_generation = state_->generation;
  auto cancel_previous = state_->cancel_pending;
  const auto same_leaves = [&] {
    if (state_->retired || !state_->installed || state_->inputs.size() != pinned.size()) return false;
    for (std::size_t i = 0; i < pinned.size(); ++i)
      if (state_->inputs[i].leaf != pinned[i].leaf) return false;
    return true;
  };
  delivery.unlock();
  if (cancel_previous) cancel_previous();
  delivery.lock();
  if (!same_leaves() || state_->generation != initial_generation) {
    delivery.unlock();
    return Reply::Err(error("driver_rebind_membership_changed"));
  }
  if (!state_->Advance())
    return Reply::Err(error("driver_generation_exhausted"));
  const auto generation = state_->generation;
  auto retired_rebind = std::move(state_->last_rebind);
  state_->last_rebind = inputs;
  state_->last_rebind_error.reset();
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    delivery.unlock();
    auto rebound = std::visit(
        [&](auto &leaf) -> core::Result<void> {
          core::Result<void> result = [&]() -> core::Result<void> {
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(leaf)>,
                              std::shared_ptr<GeometricContactInputProvider>>) {
              if (!inputs[i].geometric_contact)
                return Reply::Err(
                    error("driver_geometric_contact_binding_invalid"));
              auto binding = *inputs[i].geometric_contact;
              binding.incarnation = inputs[i].incarnation;
              binding.signals.clear();
              for (const auto &row : inputs[i].rows)
                binding.signals.push_back(row.descriptor);
              return leaf->RebindIngress(inputs[i].ingress,
                                         std::move(binding),
                                         generation);
            } else {
              const auto result = leaf->RebindIngress(
                  inputs[i].ingress, inputs[i].incarnation,
                  generation);
              return result.ok() ? Reply::Ok()
                                 : Reply::Err(result.error());
            }
          }();
          if (!result.ok())
            return Reply::Err(result.error());
          return Reply::Ok();
        },
        pinned[i].leaf);
    delivery.lock();
    if (!same_leaves() || state_->generation != generation) {
      delivery.unlock();
      return Reply::Err(error("driver_rebind_membership_changed"));
    }
    if (!rebound.ok()) {
      state_->last_rebind_error = rebound.error();
      auto owner = state_->ros_owner.load(std::memory_order_acquire);
      const bool ros_leaf =
          std::holds_alternative<std::shared_ptr<RosBooleanInputProvider>>(
              state_->inputs[i].leaf) ||
          std::holds_alternative<
              std::shared_ptr<RosMappedBooleanInputProvider>>(
              state_->inputs[i].leaf);
      if (!ros_leaf || !owner ||
          state_->inputs[i].installation.node != owner->node() ||
          !is_ros_owner_refusal(rebound.error()))
        owner.reset();
      state_->RecordFault(rebound.error(), owner);
      state_->state = core::ComponentLifecycleStateDTO::kFaulted;
      delivery.unlock();
      pinned.clear();
      retired_installations.clear();
      retired_rebind.reset();
      cancel_previous = {};
      delivery.lock();
      return rebound;
    }
    retired_installations.push_back(std::move(state_->inputs[i].installation));
    state_->inputs[i].installation = inputs[i];
  }
  auto retired_cancel = std::move(state_->cancel_pending);
  auto retired_lifecycle = std::move(state_->input_lifecycle);
  auto retired_acquisition = std::move(state_->input_acquisition);
  state_->cancel_pending = std::move(cancel_pending_answers);
  state_->input_lifecycle = std::move(input_lifecycle);
  state_->input_acquisition = std::move(input_acquisition);
  delivery.unlock();
  pinned.clear();
  retired_installations.clear();
  retired_rebind.reset();
  cancel_previous = {};
  retired_cancel = {};
  retired_lifecycle = {};
  retired_acquisition = {};
  delivery.lock();
  return Reply::Ok();
}

std::size_t IoProviderDriverRecord::RosInputMatchedPublishersForTest() const {
  std::size_t total = 0U;
  state_->ForRosInputs([&](const auto &, auto &leaf) {
    total += leaf.matched_publishers().value_or(0U);
  });
  return total;
}

core::ComponentState IoProviderDriverRecord::State(const std::int64_t now_ns) const {
  using Lifecycle = core::ComponentLifecycleStateDTO;
  if (const auto owner = state_->ros_owner.load(std::memory_order_acquire)) {
    if (const auto stalled = owner->owner_stall()) {
      core::ComponentState result;
      result.observed_at_ns = now_ns;
      result.lifecycle = core::ComponentLifecycle::kFault;
      result.activity = core::ComponentActivity::kUnknown;
      result.fault = core::Fault{
          core::FaultCode::kOwnerStalled,
          "ROS transport owner " + stalled->owner + " stalled in " + stalled->task,
          true, core::FaultOperatorAction::kRecover, std::nullopt, std::nullopt, {}, now_ns};
      return result;
    }
  }
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  state.activity = core::ComponentActivity::kIdle;
  const auto component = state_->domain.provider()->provider.controllable_component_id;
  const auto healths = host_component_stream_health()->Read(component);
  for (const auto &stream : healths)
    state.streams.push_back(stream_of(stream));
  auto lock = AcquireDelivery();
  const auto fault = [&](core::FaultCode code, const core::ErrorDTO &cause,
                         core::FaultOperatorAction action) {
    state.lifecycle = core::ComponentLifecycle::kFault;
    state.fault = core::Fault{code,
                              cause.message,
                              action != core::FaultOperatorAction::kNone,
                              action,
                              std::nullopt,
                              std::nullopt,
                              {},
                              now_ns};
    return state;
  };
  if (state_->retired || !state_->installed) {
    state.lifecycle = core::ComponentLifecycle::kNotPrepared;
    return state;
  }
  if (const auto exited = state_->ProcessFault())
    return fault(core::FaultCode::kProcessExited, *exited, core::FaultOperatorAction::kNone);
  if (state_->fault)
    return fault(core::FaultCode::kCommunication, *state_->fault,
                 core::FaultOperatorAction::kRecover);
  const bool transport = state_->TransportReady();
  if (state_->enabled && !transport)
    return fault(core::FaultCode::kCommunication,
                 state_->TransportDiagnostic().value_or(
                     error("driver_transport_lost")),
                 core::FaultOperatorAction::kRecover);
  if (!transport) {
    const auto diagnostic =
        state_->TransportDiagnostic().value_or(error("driver_transport_unavailable"));
    state.lifecycle = core::ComponentLifecycle::kOffline;
    state.reason = core::ComponentStateReason{diagnostic.code, diagnostic.message};
    return state;
  }
  // IO-1: lost senders fault an enabled device until Recover; a device that
  // isn't enabled is offline while they're gone (as a lost transport).
  const bool gone = state_->SendersGone();
  if (gone && state_->enabled) {
    state_->senders_lost = true;
    state_->state = Lifecycle::kFaulted;
  }
  if (state_->senders_lost)
    return fault(core::FaultCode::kCommunication, senders_gone(),
                 core::FaultOperatorAction::kRecover);
  if (gone) {
    state.lifecycle = core::ComponentLifecycle::kOffline;
    state.reason = core::ComponentStateReason{senders_gone().code, senders_gone().message};
    return state;
  }
  // Stream meaning by kind: only a `fault` stream going stale faults
  // the component; a pose source's or AI model's (`warning`) stream only
  // shows in `streams[].freshness`.
  for (const auto &stream : healths)
    if (stream.state == core::StreamLivenessStateDTO::kStale &&
        stream.severity == core::StreamSeverityDTO::kFault)
      return fault(core::FaultCode::kStreamStale,
                   {"fault_stream_stale", "Stream " + stream.stream_id + " stopped reporting", {}},
                   core::FaultOperatorAction::kNone);
  state.lifecycle = state_->state == Lifecycle::kActive     ? core::ComponentLifecycle::kEnabled
                    : state_->state == Lifecycle::kDeclared ? core::ComponentLifecycle::kNotPrepared
                                                            : core::ComponentLifecycle::kPrepared;
  return state;
}

std::vector<core::Capability> IoProviderDriverRecord::capabilities() const {
  auto lock = AcquireDelivery();
  if (state_->outputs.empty()) return {};
  // Mock outputs report the written value back.
  if (std::any_of(state_->outputs.begin(), state_->outputs.end(), [](const auto &output) {
        return std::holds_alternative<std::shared_ptr<MockBooleanOutputProvider>>(
            output.provider);
      }))
    return {core::Capability::kIoManualWrite, core::Capability::kIoOutputReadback};
  return {core::Capability::kIoManualWrite};
}

SetOutputResult IoProviderDriverRecord::SetOutput(const std::string_view signal_name,
                                                 const std::variant<bool, double> value,
                                                 const std::stop_token stop) {
  const auto failed = [&](std::string code, std::string message) {
    return SetOutputResult{core::ActionOutcome::kFailed,
                           core::ActionError{std::move(code), std::move(message),
                                             std::nullopt, std::nullopt}};
  };
  if (!std::holds_alternative<bool>(value))
    return failed("io_signal_type_mismatch",
                  std::string(signal_name) + " is a digital output; its value is true or false");
  const core::IoSignalId signal{state_->domain.provider()->provider.controllable_component_id,
                                std::string(signal_name)};
  std::variant<std::shared_ptr<MockBooleanOutputProvider>,
               std::shared_ptr<RosBooleanOutputProvider>>
      provider;
  bool found = false;
  bool controller_owned = false;
  {
    auto lock = AcquireDelivery();
    if (state_->ros_owner_recovery_closed)
      return failed("io_output_not_submitted",
                    "the I/O transport is recovering; nothing was sent");
    if (!state_->output_admission_open)
      return failed("io_output_not_submitted",
                    "the I/O component is disabling or disabled; nothing was sent");
    for (const auto &output : state_->outputs)
      for (const auto &row : output.rows)
        if (row.descriptor.signal_id == signal) {
          provider = output.provider;
          found = true;
          controller_owned = row.descriptor.controller_owned;
        }
  }
  if (!found)
    return failed("io_signal_unknown",
                  "'" + signal.name + "' is not an output of this component");
  // A controller-owned output is refused before sending.
  if (controller_owned)
    return failed("io_output_controller_owned",
                  signal.name + " is driven by the controller; ManyForge can't set it");
  // The provider answers outside the delivery barrier (a service write waits).
  const bool written = std::get<bool>(value);
  const auto *mock = std::get_if<std::shared_ptr<MockBooleanOutputProvider>>(&provider);
  auto result = mock != nullptr
                    ? (*mock)->WriteOutput(signal, written)
                    : std::get<std::shared_ptr<RosBooleanOutputProvider>>(provider)->WriteOutput(
                          signal, written, stop);
  return result;
}

core::ActionResult IoProviderDriverRecord::Run(const core::ComponentAction action,
                                               const std::stop_token stop) {
  using Lifecycle = core::ComponentLifecycleStateDTO;
  using Outcome = core::ActionOutcome;
  const auto refused = [](std::string reason) {
    return action_result(Outcome::kFailed, error(std::move(reason)));
  };
  auto lock = AcquireDelivery();
  if (stop.stop_requested())
    return refused("driver_action_interrupted");
  if (state_->retired || !state_->installed)
    return refused("driver_resources_absent");
  const bool prepare = action == core::ComponentAction::kPrepare;
  const bool enable = action == core::ComponentAction::kEnable;
  const bool disable = action == core::ComponentAction::kDisable;
  const bool recover = action == core::ComponentAction::kRecover;
  if (!prepare && !enable && !disable && !recover)
    return refused("driver_action_unsupported");
  if (!disable && state_->ros_owner_recovery_closed)
    return refused("driver_ros_owner_recovery_in_progress");
  // An already satisfied Enable succeeds at once.
  if (enable && state_->state == Lifecycle::kActive)
    return action_result(Outcome::kSucceeded);
  if (enable && state_->state != Lifecycle::kPrepared && state_->state != Lifecycle::kDisabled)
    return refused("driver_not_prepared");
  if ((prepare || enable) && (state_->fault || state_->senders_lost))
    return refused("driver_fault_present");
  if (!disable && state_->ProcessFault())
    return refused("driver_process_exited");
  if (!disable && !state_->TransportReady())
    return refused("driver_transport_unavailable");
  if (!disable && state_->SendersGone())
    return action_result(Outcome::kFailed, senders_gone());
  if (state_->generation == std::numeric_limits<std::uint64_t>::max())
    return refused("driver_generation_exhausted");
  if (disable) state_->output_admission_open = false;
  // Pin the admitted lifecycle's exact leaves across owner hand-offs. Delivery
  // callbacks must be able to acquire the data mutex while those run.
  const auto inputs = state_->inputs;
  auto outputs = state_->outputs;
  const auto initial_generation = state_->generation;
  const auto lifecycle = state_->input_lifecycle;
  const auto acquisition = state_->input_acquisition;
  const auto cancel_pending = state_->cancel_pending;
  const auto occurrence = state_->occurrence;
  const auto same_inputs = [&] {
    if (state_->retired || !state_->installed || state_->inputs.size() != inputs.size() ||
        state_->outputs.size() != outputs.size()) return false;
    for (std::size_t i = 0; i < inputs.size(); ++i)
      if (state_->inputs[i].leaf != inputs[i].leaf) return false;
    for (std::size_t i = 0; i < outputs.size(); ++i)
      if (state_->outputs[i].provider != outputs[i].provider) return false;
    return true;
  };
  lock.unlock();
  if ((enable || disable || recover) && !inputs.empty() && lifecycle) {
    const auto planned = lifecycle(!enable);
    lock.lock();
    if (!same_inputs() || state_->generation != initial_generation) {
      lock.unlock();
      return action_result(Outcome::kUnknown, error("driver_rebind_membership_changed"));
    }
    if (!planned.ok()) {
      if (planned.error().details.contains("io_source_transition_effects")) {
        static_cast<void>(
            state_->Advance()); // Capacity was checked before the callback.
        state_->RecordFault(planned.error());
        state_->state = Lifecycle::kFaulted;
      } else if (disable && state_->state == Lifecycle::kActive && state_->enabled) {
        state_->output_admission_open = true;
      }
      return action_result(Outcome::kFailed, planned.error());
    }
  } else {
    lock.lock();
    if (!same_inputs() || state_->generation != initial_generation) {
      lock.unlock();
      return refused("driver_rebind_membership_changed");
    }
  }
  if (!state_->Advance())
    return refused("driver_generation_exhausted");
  const auto generation = state_->generation;
  lock.unlock();
  if (cancel_pending)
    cancel_pending();
  std::optional<core::ErrorDTO> failure;
  std::optional<core::ErrorDTO> owner_refusal;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources>
      owner_refusal_origin;
  std::vector<IoProviderInputAcquisitionBaseline> acquisition_baselines;
  for (const auto &input : inputs) {
    bool accepted = false;
    try {
      accepted = std::visit(
          [&](const auto &leaf) {
            if (!enable)
              return leaf->Pause(generation);
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(leaf)>,
                              std::shared_ptr<MockBooleanInputProvider>>) {
              const auto resumed = leaf->Resume(generation);
              // Connection and actual acquisition delivery are independent
              // facts. A refused Mock baseline remains unavailable through
              // shared ingress.
              return resumed.ok() || leaf->status().accepting;
            } else if constexpr (std::is_same_v<
                                     std::decay_t<decltype(leaf)>,
                                     std::shared_ptr<GeometricContactInputProvider>>) {
              const auto resumed = leaf->Resume(generation);
              if (!resumed.ok())
                return false;
              std::vector<core::IoSignalId> signals;
              signals.reserve(input.installation.rows.size());
              for (const auto &row : input.installation.rows)
                signals.push_back(row.descriptor.signal_id);
              acquisition_baselines.push_back({resumed.value().incarnation,
                                               resumed.value().source_sequence,
                                               std::move(signals)});
              return true;
            } else
              return leaf->Resume(generation);
          },
          input.leaf);
    } catch (const core::TypedError &error) {
      if (!(error.code() == "ros_transport_executor_failed" ||
            (error.code() == "component_not_ready" &&
             error.details().contains("reason") &&
             error.details().at("reason") == "owner_closing")))
        throw;
      owner_refusal = error.error();
      const bool ros_leaf =
          std::holds_alternative<std::shared_ptr<RosBooleanInputProvider>>(
              input.leaf) ||
          std::holds_alternative<
              std::shared_ptr<RosMappedBooleanInputProvider>>(input.leaf);
      auto owner = state_->ros_owner.load(std::memory_order_acquire);
      if (ros_leaf && owner && input.installation.node == owner->node())
        owner_refusal_origin = std::move(owner);
      else
        owner_refusal_origin.reset();
    }
    if (!accepted)
      failure = error("driver_input_gate_failed");
  }
  if (enable && !failure && acquisition) {
    const auto acquired = acquisition(acquisition_baselines);
    if (!acquired.ok())
      failure = acquired.error();
  }
  for (auto &output : outputs) {
    const auto result = std::visit(
        [&](auto &provider) {
          return enable ? provider->Resume(occurrence, generation,
                                           weak_from_this())
                        : provider->Pause(occurrence, generation,
                                          weak_from_this());
        },
        output.provider);
    if (!result.ok())
      failure = result.error();
  }
  if (owner_refusal)
    failure = std::move(owner_refusal);
  if (enable && failure)
    for (const auto &input : inputs)
      std::visit(
          [&](const auto &leaf) {
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(leaf)>,
                              std::shared_ptr<GeometricContactInputProvider>>)
              static_cast<void>(leaf->AbortResume(generation));
          },
          input.leaf);
  lock.lock();
  if (!same_inputs() || state_->generation != generation) {
    lock.unlock();
    return action_result(Outcome::kUnknown, error("driver_rebind_membership_changed"));
  }
  if (failure) {
    state_->RecordFault(*failure, owner_refusal_origin);
    state_->state = Lifecycle::kFaulted;
    return action_result(Outcome::kFailed, failure);
  }
  state_->enabled = enable;
  if (enable) state_->output_admission_open = true;
  state_->ClearFault();
  state_->senders_lost = false;
  state_->state = enable ? Lifecycle::kActive : Lifecycle::kPrepared;
  return action_result(Outcome::kSucceeded);
}

void IoProviderDriverRecord::SetSupervisedProcessFaultSource(
    std::function<std::optional<core::ErrorDTO>()> source) {
  auto lock = AcquireDelivery();
  state_->process_fault_source = std::move(source);
}

bool IoProviderDriverRecord::stream_observer() const noexcept {
  const auto &id = state_->manifest.provider_manifest_id.value();
  return id == kPoseSourceProviderManifestId || id == kAiModelProviderManifestId;
}
bool IoProviderDriverRecord::InstalledStreamObserver() const noexcept {
  return state_->observer_installed.load(std::memory_order_acquire);
}

core::Result<void>
IoProviderDriverRecord::SetInputsStreamLocked(const std::optional<std::int64_t> budget_ns) {
  using Reply = core::Result<void>;
  const bool ros = state_->ForRosInputs([](const auto &, auto &) {});
  if (!budget_ns || !ros || state_->retired) {
    state_->ForRosInputs([](const auto &, auto &leaf) { leaf.SetArrivalRecorder(nullptr); });
    state_->inputs_stream.reset();
    return Reply::Ok();
  }
  if (!state_->inputs_stream) {
    // An I/O device's own stream going stale is its fault.
    auto declared = host_component_stream_health()->Declare(
        state_->domain.provider()->provider.controllable_component_id,
        {std::string(kIoDeviceInputsStreamId), *budget_ns, core::StreamSeverityDTO::kFault,
         core::StreamRecoveryDTO::kAuto, core::StreamTimestampSourceDTO::kArrival});
    if (!declared.ok())
      return Reply::Err(declared.error());
    state_->inputs_stream = std::move(declared.value());
  }
  state_->ForRosInputs(
      [&](const auto &, auto &leaf) { leaf.SetArrivalRecorder(state_->inputs_stream); });
  return Reply::Ok();
}

core::Result<void> IoProviderDriverRecord::InstallObserverStreamsLocked(
    std::vector<std::unique_ptr<core::StreamRecorder>> streams) {
  using Reply = core::Result<void>;
  if (!stream_observer() || state_->installed || state_->retired)
    return Reply::Err(error("driver_pose_source_install_invalid"));
  std::vector<std::string> ids;
  for (const auto &stream : streams) {
    if (!stream)
      return Reply::Err(error("driver_pose_source_install_invalid"));
    ids.push_back(stream->stream_id());
  }
  std::sort(ids.begin(), ids.end());
  if (ids != state_->domain.provider()->provider.signal_names)
    return Reply::Err(error("driver_resource_closure_not_exact"));
  if (!state_->Advance())
    return Reply::Err(error("driver_generation_exhausted"));
  state_->observer_streams = std::move(streams);
  state_->installed = true;
  state_->observer_installed.store(true, std::memory_order_release);
  return Reply::Ok();
}

void IoProviderDriverRecord::Shutdown() {
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> retired_owner;
  auto lock = AcquireDelivery();
  if (state_->retired)
    return;
  state_->retired = true;
  state_->output_admission_open = false;
  state_->observer_installed.store(false, std::memory_order_release);
  retired_owner = state_->ros_owner.exchange({}, std::memory_order_acq_rel);
  auto inputs = std::move(state_->inputs);
  auto outputs = std::move(state_->outputs);
  auto input_stream = std::move(state_->inputs_stream);
  auto observer_streams = std::move(state_->observer_streams);
  static_cast<void>(input_stream);
  static_cast<void>(observer_streams);
  const auto cancel_pending = state_->cancel_pending;
  const auto occurrence = state_->occurrence;
  if (!state_->Advance())
    state_->RecordFault(error("driver_generation_exhausted"));
  state_->enabled = false;
  state_->state = core::ComponentLifecycleStateDTO::kDisabled;
  lock.unlock();
  std::optional<core::ErrorDTO> failure;
  try {
    if (cancel_pending)
      cancel_pending();
  } catch (...) {
    // A faulty timer-cancellation hook cannot keep retired leaves accepting.
    failure = error("driver_timer_cancellation_failed");
  }
  for (auto &input : inputs)
    std::visit([](auto &leaf) { leaf->Shutdown(); }, input.leaf);
  // The retired leaves no longer hold the `inputs` stream (undeclared).
  for (auto &input : inputs)
    std::visit([](auto &leaf) {
      using Leaf = std::decay_t<decltype(leaf)>;
      if constexpr (std::is_same_v<Leaf, std::shared_ptr<RosBooleanInputProvider>> ||
                    std::is_same_v<Leaf, std::shared_ptr<RosMappedBooleanInputProvider>>)
        leaf->SetArrivalRecorder(nullptr);
    }, input.leaf);
  // Permanent retirement fences the exact occurrence independently of its
  // generation range. Other channel aliases and pending ACKs stay owned by
  // their existing lifecycle/session owners.
  for (auto &output : outputs) {
    const auto fenced = std::visit(
        [&](auto &provider) {
          return provider->ShutdownOccurrence(occurrence,
                                              weak_from_this());
        },
        output.provider);
    if (!fenced.ok())
      failure = fenced.error();
  }
  if (failure) {
    lock.lock();
    state_->RecordFault(*failure);
    lock.unlock();
  }
}

} // namespace manyforge::roboplan_backend::host_detail
