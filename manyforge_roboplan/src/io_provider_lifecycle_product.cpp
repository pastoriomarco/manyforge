#include "io_provider_lifecycle_product.hpp"
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/ros_mapped_boolean_input_provider.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <variant>
#include <type_traits>

namespace manyforge::roboplan_backend::host_detail {
namespace {
core::ErrorDTO error(std::string reason) {
  return {"io_provider_lifecycle_unavailable",
          "The actual I/O driver cannot satisfy this request",
          {{"reason", std::move(reason)}}};
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
    std::variant<std::unique_ptr<MockBooleanInputProvider>,
                 std::unique_ptr<RosBooleanInputProvider>,
                 std::unique_ptr<RosMappedBooleanInputProvider>,
                 std::unique_ptr<GeometricContactInputProvider>>
        leaf;
  };
  core::ResolvedLifecycleControlDomainScopeDTO domain;
  core::ResolvedLifecycleProviderOccurrenceDTO occurrence;
  core::ProviderManifestDTO manifest;
  std::shared_ptr<core::IClock> clock;
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
  bool retired{};
  std::optional<core::ErrorDTO> fault;
  /// Outputs whose last write's outcome is unknown (CONTRACTS §7): the
  /// component's activity is `unknown` until a later answered write to that
  /// output, or a successful prepare or recover.
  std::set<std::string> unknown_outputs;
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
            if constexpr (std::is_same_v<Leaf, std::unique_ptr<RosBooleanInputProvider>> ||
                          std::is_same_v<Leaf, std::unique_ptr<RosMappedBooleanInputProvider>>) {
              any = true;
              visit(input, *leaf);
            }
          },
          input.leaf);
    return any;
  }

  // IO-1: every ROS input topic has no publisher, after one had one since
  // install. The graph's count per topic, so a leaf's resubscription
  // (Enable) doesn't look like a lost sender.
  bool SendersGone() {
    bool present = false;
    const bool ros = ForRosInputs([&](const Input &input, auto &) {
      const auto &topic = input.installation.rows.front().provider.topic;
      try {
        present = present || input.installation.node->count_publishers(*topic) > 0U;
      } catch (const std::exception &) {
        // The ROS context is going away; TransportReady() reports it.
        present = true;
      }
    });
    senders_seen = senders_seen || present;
    return ros && senders_seen && !present;
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
          !input.installation.node->get_node_base_interface()
               ->get_context()
               ->is_valid())
        return false;
    for (const auto &output : outputs) {
      const bool ready = std::visit(
          [](const auto &provider) {
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(provider)>,
                              std::shared_ptr<RosBooleanOutputProvider>>)
              return provider && provider->ServiceReady();
            else
              return static_cast<bool>(provider);
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
          !input.installation.node->get_node_base_interface()
               ->get_context()
               ->is_valid())
        return missing(input.installation.rows, "input_topic",
                       input.installation.rows.empty()
                           ? std::nullopt
                           : input.installation.rows.front().provider.topic);
    for (const auto &output : outputs) {
      const auto *ros =
          std::get_if<std::shared_ptr<RosBooleanOutputProvider>>(
              &output.provider);
      const bool ready =
          ros ? *ros && (*ros)->ServiceReady()
              : static_cast<bool>(std::get<0>(output.provider));
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
std::unique_lock<std::mutex> IoProviderDriverRecord::AcquireDelivery() const {
  return std::unique_lock(state_->mutex);
}
std::uint64_t
IoProviderDriverRecord::driver_observation_generation_locked() const noexcept {
  return state_->generation;
}
core::Result<void> IoProviderDriverRecord::InstallResourcesLocked(
    std::vector<IoProviderInputSourceInstallation> inputs,
    std::vector<IoProviderOutputChannelInstallation> outputs,
    std::function<void()> cancel_pending_answers,
    InputLifecycleHook input_lifecycle,
    InputAcquisitionHook input_acquisition) {
  using Reply = core::Result<void>;
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
  std::vector<Driver::Input> prepared_inputs;
  prepared_inputs.reserve(inputs.size());
  for (auto &input : inputs) {
    const auto &first = input.rows.front();
    if (first.provider.profile == "mock") {
      std::vector<MockBooleanInput> signals;
      for (const auto &row : input.rows) {
        if (!row.provider.initial_value)
          return Reply::Err(error("driver_mock_initial_value_absent"));
        signals.push_back({row.descriptor, *row.provider.initial_value});
      }
      auto leaf = MockBooleanInputProvider::Create(
          input.ingress, *state_->clock,
          {first.descriptor.source_id, input.incarnation, std::move(signals)},
          state_->generation);
      if (!leaf.ok())
        return Reply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value())});
    } else if (first.provider.profile == "ros_bool") {
      if (!input.node || input.rows.size() != 1U || !first.provider.topic)
        return Reply::Err(error("driver_ros_bool_group_unsupported"));
      auto leaf = RosBooleanInputProvider::Create(
          input.node, input.ingress, *state_->clock,
          {first.descriptor.signal_id, first.descriptor.source_id,
           input.incarnation, *first.provider.topic},
          first.descriptor,
          rclcpp::QoS(rclcpp::KeepLast(first.provider.ingress_depth)),
          state_->generation);
      if (!leaf.ok())
        return Reply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value())});
    } else if (first.provider.profile == "geometric_contact") {
      if (!input.geometric_contact || !input.geometric_contact->scheduler)
        return Reply::Err(error("driver_geometric_contact_binding_invalid"));
      auto binding = *input.geometric_contact;
      binding.incarnation = input.incarnation;
      binding.signals.clear();
      for (const auto &row : input.rows)
        binding.signals.push_back(row.descriptor);
      const auto scheduler = binding.scheduler;
      auto leaf = GeometricContactInputProvider::Create(
          input.ingress, *state_->clock, *scheduler, std::move(binding),
          state_->generation);
      if (!leaf.ok())
        return Reply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value())});
    } else if (first.provider.profile == "ros2_control_gpio") {
      if (!input.node || !first.provider.topic)
        return Reply::Err(error("driver_ros_mapped_source_invalid"));
      std::vector<RosMappedBooleanInputSignal> signals;
      signals.reserve(input.rows.size());
      for (const auto &row : input.rows) {
        if (!row.provider.group || !row.provider.interface_name)
          return Reply::Err(error("driver_gpio_input_selector_absent"));
        signals.push_back(
            {row.descriptor,
             RosGpioBooleanInputSelector{*row.provider.group,
                                         *row.provider.interface_name}});
      }
      auto leaf = RosMappedBooleanInputProvider::Create(
          input.node, input.ingress, *state_->clock,
          {first.descriptor.source_id, input.incarnation, *first.provider.topic,
           std::move(signals)},
          rclcpp::QoS(rclcpp::KeepLast(first.provider.ingress_depth)),
          state_->generation);
      if (!leaf.ok())
        return Reply::Err(leaf.error());
      prepared_inputs.push_back({std::move(input), std::move(leaf.value())});
    } else
      return Reply::Err(error("driver_input_profile_unsupported"));
  }
  if (!state_->Advance())
    return Reply::Err(error("driver_generation_exhausted"));
  state_->inputs = std::move(prepared_inputs);
  state_->outputs = std::move(outputs);
  state_->cancel_pending = std::move(cancel_pending_answers);
  state_->input_lifecycle = std::move(input_lifecycle);
  state_->input_acquisition = std::move(input_acquisition);
  state_->installed = true;
  return Reply::Ok();
}

core::Result<MockBooleanInputSetReceipt>
IoProviderDriverRecord::SetMockValueLocked(const core::IoSignalId &signal,
                                           bool value) {
  for (auto &input : state_->inputs)
    if (const auto *mock =
            std::get_if<std::unique_ptr<MockBooleanInputProvider>>(&input.leaf))
      if (std::any_of(input.installation.rows.begin(),
                      input.installation.rows.end(), [&](const auto &row) {
                        return row.descriptor.signal_id == signal;
                      }))
        return (*mock)->SetValue(signal, value);
  return core::Result<MockBooleanInputSetReceipt>::Err(
      error("driver_mock_signal_not_owned"));
}

core::Result<void> IoProviderDriverRecord::RebindInputsLocked(
    std::vector<IoProviderInputSourceInstallation> inputs,
    std::function<void()> cancel_pending_answers,
    InputLifecycleHook input_lifecycle,
    InputAcquisitionHook input_acquisition) {
  using Reply = core::Result<void>;
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
  if (state_->cancel_pending)
    state_->cancel_pending();
  if (!state_->Advance())
    return Reply::Err(error("driver_generation_exhausted"));
  state_->last_rebind = inputs;
  state_->last_rebind_error.reset();
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    auto rebound = std::visit(
        [&](auto &leaf) -> core::Result<void> {
          core::Result<void> result = [&]() -> core::Result<void> {
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(leaf)>,
                              std::unique_ptr<GeometricContactInputProvider>>) {
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
                                         state_->generation);
            } else {
              const auto result = leaf->RebindIngress(
                  inputs[i].ingress, inputs[i].incarnation,
                  state_->generation);
              return result.ok() ? Reply::Ok()
                                 : Reply::Err(result.error());
            }
          }();
          if (!result.ok())
            return Reply::Err(result.error());
          return Reply::Ok();
        },
        state_->inputs[i].leaf);
    if (!rebound.ok()) {
      state_->last_rebind_error = rebound.error();
      state_->fault = rebound.error();
      state_->state = core::ComponentLifecycleStateDTO::kFaulted;
          return rebound;
    }
    state_->inputs[i].installation = inputs[i];
  }
  state_->cancel_pending = std::move(cancel_pending_answers);
  state_->input_lifecycle = std::move(input_lifecycle);
  state_->input_acquisition = std::move(input_acquisition);
  return Reply::Ok();
}

core::ComponentState IoProviderDriverRecord::State(const std::int64_t now_ns) const {
  using Lifecycle = core::ComponentLifecycleStateDTO;
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
  // CONTRACTS §7 (stream meaning by kind): only a `fault` stream going stale faults
  // the component; a pose source's or AI model's (`warning`) stream only
  // shows in `streams[].freshness`.
  for (const auto &stream : healths)
    if (stream.state == core::StreamLivenessStateDTO::kStale &&
        stream.severity == core::StreamSeverityDTO::kFault)
      return fault(core::FaultCode::kStreamStale,
                   {"fault_stream_stale", "Stream " + stream.stream_id + " stopped reporting", {}},
                   core::FaultOperatorAction::kNone);
  if (!state_->unknown_outputs.empty())
    state.activity = core::ComponentActivity::kUnknown;
  state.lifecycle = state_->state == Lifecycle::kActive     ? core::ComponentLifecycle::kEnabled
                    : state_->state == Lifecycle::kDeclared ? core::ComponentLifecycle::kNotPrepared
                                                            : core::ComponentLifecycle::kPrepared;
  return state;
}

std::vector<core::Capability> IoProviderDriverRecord::capabilities() const {
  auto lock = AcquireDelivery();
  if (state_->outputs.empty()) return {};
  // Mock outputs report the written value back (CONTRACTS §7, D2).
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
  // CONTRACTS §7: a controller-owned output is refused before sending.
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
  // An unknown outcome leaves the component's activity unknown (CONTRACTS
  // §7); an answered write to the same output ends it.
  auto lock = AcquireDelivery();
  if (result.outcome == core::ActionOutcome::kUnknown)
    state_->unknown_outputs.insert(signal.name);
  else
    state_->unknown_outputs.erase(signal.name);
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
  // CONTRACTS §2: an already satisfied Enable succeeds at once.
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
  if ((enable || disable || recover) && !state_->inputs.empty() &&
      state_->input_lifecycle) {
    const auto planned = state_->input_lifecycle(!enable);
    if (!planned.ok()) {
      if (planned.error().details.contains("io_source_transition_effects")) {
        static_cast<void>(
            state_->Advance()); // Capacity was checked before the callback.
        state_->fault = planned.error();
        state_->state = Lifecycle::kFaulted;
      }
      return action_result(Outcome::kFailed, planned.error());
    }
  }
  if (!state_->Advance())
    return refused("driver_generation_exhausted");
  if (state_->cancel_pending)
    state_->cancel_pending();
  const auto &occurrence = state_->occurrence;
  std::optional<core::ErrorDTO> failure;
  std::vector<IoProviderInputAcquisitionBaseline> acquisition_baselines;
  for (auto &input : state_->inputs) {
    const bool accepted = std::visit(
        [&](auto &leaf) {
          if (!enable)
            return leaf->Pause(state_->generation);
          if constexpr (std::is_same_v<
                            std::decay_t<decltype(leaf)>,
                            std::unique_ptr<MockBooleanInputProvider>>) {
            const auto resumed = leaf->Resume(state_->generation);
            // Connection and actual acquisition delivery are independent facts.
            // A refused Mock baseline remains unavailable through shared
            // ingress.
            return resumed.ok() || leaf->status().accepting;
          } else if constexpr (std::is_same_v<
                                   std::decay_t<decltype(leaf)>,
                                   std::unique_ptr<
                                       GeometricContactInputProvider>>) {
            const auto resumed = leaf->Resume(state_->generation);
            if (!resumed.ok())
              return false;
            std::vector<core::IoSignalId> signals;
            signals.reserve(input.installation.rows.size());
            for (const auto &row : input.installation.rows)
              signals.push_back(row.descriptor.signal_id);
            acquisition_baselines.push_back(
                {resumed.value().incarnation,
                 resumed.value().source_sequence, std::move(signals)});
            return true;
          } else
            return leaf->Resume(state_->generation);
        },
        input.leaf);
    if (!accepted)
      failure = error("driver_input_gate_failed");
  }
  if (enable && !failure && state_->input_acquisition) {
    const auto acquired = state_->input_acquisition(acquisition_baselines);
    if (!acquired.ok())
      failure = acquired.error();
  }
  for (auto &output : state_->outputs) {
    const auto result = std::visit(
        [&](auto &provider) {
          return enable ? provider->Resume(occurrence, state_->generation,
                                           weak_from_this())
                        : provider->Pause(occurrence, state_->generation,
                                          weak_from_this());
        },
        output.provider);
    if (!result.ok())
      failure = result.error();
  }
  if (enable && failure)
    for (auto &input : state_->inputs)
      std::visit(
          [&](auto &leaf) {
            if constexpr (std::is_same_v<
                              std::decay_t<decltype(leaf)>,
                              std::unique_ptr<
                                  GeometricContactInputProvider>>)
              static_cast<void>(leaf->AbortResume(state_->generation));
          },
          input.leaf);
  if (failure) {
    state_->fault = failure;
    state_->state = Lifecycle::kFaulted;
    return action_result(Outcome::kFailed, failure);
  }
  state_->enabled = enable;
  state_->fault.reset();
  state_->senders_lost = false;
  if (prepare || recover)
    state_->unknown_outputs.clear();
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
    // CONTRACTS §7: an I/O device's own stream going stale is its fault.
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
  return Reply::Ok();
}

void IoProviderDriverRecord::Shutdown() {
  auto lock = AcquireDelivery();
  if (state_->retired)
    return;
  state_->retired = true;
  try {
    if (state_->cancel_pending)
      state_->cancel_pending();
  } catch (...) {
    // A faulty timer-cancellation hook cannot keep retired leaves accepting.
    state_->fault = error("driver_timer_cancellation_failed");
  }
  if (!state_->Advance())
    state_->fault = error("driver_generation_exhausted");
  for (auto &input : state_->inputs)
    std::visit([](auto &leaf) { leaf->Shutdown(); }, input.leaf);
  // The retired leaves no longer hold the `inputs` stream (undeclared).
  state_->ForRosInputs([](const auto &, auto &leaf) { leaf.SetArrivalRecorder(nullptr); });
  state_->inputs_stream.reset();
  // Permanent retirement fences the exact occurrence independently of its
  // generation range. Other channel aliases and pending ACKs stay owned by
  // their existing lifecycle/session owners.
  for (auto &output : state_->outputs) {
    const auto fenced = std::visit(
        [&](auto &provider) {
          return provider->ShutdownOccurrence(state_->occurrence,
                                              weak_from_this());
        },
        output.provider);
    if (!fenced.ok())
      state_->fault = fenced.error();
  }
  state_->observer_streams.clear();
  state_->enabled = false;
  state_->state = core::ComponentLifecycleStateDTO::kDisabled;
}

} // namespace manyforge::roboplan_backend::host_detail
