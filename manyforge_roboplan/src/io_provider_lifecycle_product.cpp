#include "io_provider_lifecycle_product.hpp"
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_roboplan/ros_mapped_boolean_input_provider.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
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

std::vector<core::LifecycleFactDTO> facts(bool resources, bool enabled,
                                          bool fault, bool transport,
                                          bool retired = false) {
  const auto boolean = [](const char *id, bool value) {
    return core::LifecycleFactDTO{
        core::LifecycleFactId(id),
        {core::LifecycleFactValueKindDTO::kBoolean, value, {}, {}}};
  };
  // Report the existing standard CCLC cause from actual local driver facts.
  // Initial unavailability and an intentionally disabled connection are not
  // faults. Retirement is ordinary teardown, even if cleanup has a diagnostic.
  const auto cause = retired ? core::LifecycleBlockingCauseClassDTO::kNone
                     : fault ? core::LifecycleBlockingCauseClassDTO::kFault
                     : enabled && !transport
                         ? core::LifecycleBlockingCauseClassDTO::kProviderLost
                     : !resources || !transport
                         ? core::LifecycleBlockingCauseClassDTO::kAvailability
                         : core::LifecycleBlockingCauseClassDTO::kNone;
  return {
      boolean("io.driver.enabled", enabled),
      boolean("io.driver.fault_present", fault),
      boolean("io.driver.resources_ready", resources),
      boolean("io.driver.transport_ready", transport),
      {core::LifecycleFactId(std::string(core::kLifecycleBlockingCauseFactId)),
       {core::LifecycleFactValueKindDTO::kToken,
        {},
        {},
        std::string(core::to_string(cause))}}};
}
} // namespace

struct IoProviderDriverRecord::State final {
  State(core::ResolvedLifecycleControlDomainScopeDTO selected_domain,
        core::ResolvedLifecycleProviderOccurrenceDTO selected_occurrence,
        core::ProviderManifestDTO selected_manifest,
        core::BrandLifecyclePlanCatalogDTO selected_catalog,
        std::shared_ptr<core::IClock> selected_clock)
      : domain(std::move(selected_domain)),
        occurrence(std::move(selected_occurrence)),
        manifest(std::move(selected_manifest)),
        catalog(std::move(selected_catalog)), clock(std::move(selected_clock)) {
  }
  struct Input final {
    IoProviderInputSourceInstallation installation;
    std::variant<std::unique_ptr<MockBooleanInputProvider>,
                 std::unique_ptr<RosBooleanInputProvider>,
                 std::unique_ptr<RosMappedBooleanInputProvider>>
        leaf;
  };
  core::ResolvedLifecycleControlDomainScopeDTO domain;
  core::ResolvedLifecycleProviderOccurrenceDTO occurrence;
  core::ProviderManifestDTO manifest;
  core::BrandLifecyclePlanCatalogDTO catalog;
  std::shared_ptr<core::IClock> clock;
  mutable std::mutex mutex;
  std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort> port;
  std::vector<Input> inputs;
  std::vector<IoProviderOutputChannelInstallation> outputs;
  std::function<void()> cancel_pending;
  InputLifecycleHook input_lifecycle;
  core::ComponentLifecycleStateDTO state{
      core::ComponentLifecycleStateDTO::kDeclared};
  std::uint64_t generation{1U};
  bool installed{};
  bool enabled{};
  bool retired{};
  std::optional<core::ErrorDTO> fault;
  std::function<std::optional<std::string>()> process_fault_source;
  std::vector<core::LifecycleFactDTO> last_facts;
  std::optional<std::vector<IoProviderInputSourceInstallation>> last_rebind;
  std::optional<core::ErrorDTO> last_rebind_error;

  // The supervised simulated PLC's exit, never latched into `fault`: the
  // restarted process's source replaces it at the next Apply.
  std::optional<core::ErrorDTO> ProcessFault() const {
    if (retired || !process_fault_source)
      return std::nullopt;
    auto reason = process_fault_source();
    if (!reason)
      return std::nullopt;
    if (reason->size() > core::kMaxLifecycleErrorMessageBytes) {
      auto end = core::kMaxLifecycleErrorMessageBytes;
      while (end > 0U &&
             (static_cast<unsigned char>((*reason)[end]) & 0xC0U) == 0x80U)
        --end;
      reason->resize(end);
    }
    return core::ErrorDTO{"simulated_plc_exited", std::move(*reason), {}};
  }

  std::optional<core::ErrorDTO> EffectiveFault() const {
    return fault ? fault : ProcessFault();
  }

  core::ComponentLifecycleStateDTO EffectiveState(
      const std::optional<core::ErrorDTO> &effective) const {
    return effective && !fault ? core::ComponentLifecycleStateDTO::kFaulted
                               : state;
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
      if (!core::ComponentIncidentDiagnosticIsValid(diagnostic))
        return std::nullopt;
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

  void CommitFacts() {
    last_facts = facts(installed && !retired, enabled,
                       EffectiveFault().has_value(), TransportReady(), retired);
  }

  core::Result<RetainedExternallyManagedDomainSnapshot> Snapshot() {
    auto effective = EffectiveFault();
    auto current = facts(installed && !retired, enabled, effective.has_value(),
                         TransportReady(), retired);
    if (current != last_facts) {
      if (!Advance())
        return core::Result<RetainedExternallyManagedDomainSnapshot>::Err(
            error("driver_generation_exhausted"));
      last_facts = current;
    }
    return core::Result<RetainedExternallyManagedDomainSnapshot>::Ok(
        {generation, EffectiveState(effective), false, std::move(current),
         std::move(effective), {}});
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

IoProviderDriverRecord::IoProviderDriverRecord(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
IoProviderDriverRecord::~IoProviderDriverRecord() { Shutdown(); }

core::Result<std::shared_ptr<IoProviderDriverRecord>>
IoProviderDriverRecord::Create(
    core::ResolvedLifecycleControlDomainScopeDTO domain,
    core::ResolvedLifecycleProviderOccurrenceDTO occurrence,
    core::ProviderManifestDTO manifest, std::shared_ptr<core::IClock> clock,
    core::Duration observation_freshness_ns) {
  using Reply = core::Result<std::shared_ptr<IoProviderDriverRecord>>;
  const auto *provider = domain.provider();
  if (!domain.locally_valid() || !provider || !clock ||
      !core::ProviderDomainMatchesOccurrence(
          domain.control_domain_id, domain.control_domain_revision,
          provider->provider, provider->provider_runtime_epoch, occurrence))
    return Reply::Err(error("driver_occurrence_invalid"));
  auto catalog = core::MakeIoProviderLifecycleCatalog(manifest);
  if (!catalog.ok())
    return Reply::Err(catalog.error());
  if (catalog.value().catalog_digest !=
          occurrence.lifecycle_plan_catalog_digest ||
      manifest.provider_manifest_id != occurrence.provider_manifest_id ||
      manifest.provider_manifest_revision !=
          occurrence.provider_manifest_revision)
    return Reply::Err(error("driver_catalog_not_exact"));
  auto state = std::make_unique<State>(
      std::move(domain), std::move(occurrence), std::move(manifest),
      std::move(catalog.value()), std::move(clock));
  state->last_facts = facts(false, false, false, false);
  auto record = std::shared_ptr<IoProviderDriverRecord>(
      new IoProviderDriverRecord(std::move(state)));
  std::weak_ptr<IoProviderDriverRecord> weak = record;
  auto port =
      RetainedExternallyManagedBrandLifecyclePort::CreateForProviderDomain(
          record->domain().provider()->provider.controllable_component_id,
          record->occurrence(),
          [weak, clock_pin = record->state_->clock](const auto &id, auto now) {
            // The borrowed clock is used by the brand port before this source
            // callback. Keep it alive even when only the port survives.
            static_cast<void>(clock_pin);
            if (auto driver = weak.lock())
              return driver->ReadLifecycleSnapshot(id, now);
            return core::Result<RetainedExternallyManagedDomainSnapshot>::Err(
                error("driver_retired"));
          },
          *record->state_->clock, observation_freshness_ns,
          [weak](const auto &intent, auto cancellation) {
            if (auto driver = weak.lock())
              return driver->Dispatch(intent, cancellation);
            return RetainedExternallyManagedPrimitiveOutcome{
                core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
                core::LifecyclePrimitiveEffectDTO::kNone,
                {},
                error("driver_retired")};
          });
  if (!port.ok())
    return Reply::Err(port.error());
  record->state_->port = std::move(port.value());
  return Reply::Ok(std::move(record));
}

const core::ResolvedLifecycleControlDomainScopeDTO &
IoProviderDriverRecord::domain() const noexcept {
  return state_->domain;
}
const core::ResolvedLifecycleProviderOccurrenceDTO &
IoProviderDriverRecord::occurrence() const noexcept {
  return state_->occurrence;
}
const core::BrandLifecyclePlanCatalogDTO &
IoProviderDriverRecord::catalog() const noexcept {
  return state_->catalog;
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
core::BrandLifecyclePortRegistrationDTO
IoProviderDriverRecord::brand_registration() const {
  std::weak_ptr<const IoProviderDriverRecord> weak = shared_from_this();
  return {state_->occurrence, state_->port,
          [weak, id = state_->domain.control_domain_id](auto now) {
            if (auto driver = weak.lock())
              return driver->ReadLifecycleHealth(id, now);
            return core::Result<core::LifecycleDomainHealthDTO>::Err(
                error("driver_retired"));
          }};
}

core::Result<void> IoProviderDriverRecord::InstallResourcesLocked(
    std::vector<IoProviderInputSourceInstallation> inputs,
    std::vector<IoProviderOutputChannelInstallation> outputs,
    std::function<void()> cancel_pending_answers,
    InputLifecycleHook input_lifecycle) {
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
              row.provider.profile == "ur_set_io" ||
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
  std::vector<State::Input> prepared_inputs;
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
    } else if (first.provider.profile == "ur_io_states" ||
               first.provider.profile == "ros2_control_gpio") {
      if (!input.node || !first.provider.topic)
        return Reply::Err(error("driver_ros_mapped_source_invalid"));
      std::vector<RosMappedBooleanInputSignal> signals;
      signals.reserve(input.rows.size());
      for (const auto &row : input.rows) {
        if (first.provider.profile == "ur_io_states") {
          if (!row.provider.pin || *row.provider.pin > 17U)
            return Reply::Err(error("driver_ur_input_pin_invalid"));
          signals.push_back(
              {row.descriptor, UrDigitalInputSelector{static_cast<std::uint8_t>(
                                   *row.provider.pin)}});
        } else {
          if (!row.provider.group || !row.provider.interface_name)
            return Reply::Err(error("driver_gpio_input_selector_absent"));
          signals.push_back(
              {row.descriptor,
               RosGpioBooleanInputSelector{*row.provider.group,
                                           *row.provider.interface_name}});
        }
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
  state_->installed = true;
  state_->CommitFacts();
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
    InputLifecycleHook input_lifecycle) {
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
          auto result = leaf->RebindIngress(
              inputs[i].ingress, inputs[i].incarnation, state_->generation);
          if (!result.ok())
            return Reply::Err(result.error());
          return Reply::Ok();
        },
        state_->inputs[i].leaf);
    if (!rebound.ok()) {
      state_->last_rebind_error = rebound.error();
      state_->fault = rebound.error();
      state_->state = core::ComponentLifecycleStateDTO::kFaulted;
      state_->CommitFacts();
      return rebound;
    }
    state_->inputs[i].installation = inputs[i];
  }
  state_->cancel_pending = std::move(cancel_pending_answers);
  state_->input_lifecycle = std::move(input_lifecycle);
  return Reply::Ok();
}

core::Result<RetainedExternallyManagedDomainSnapshot>
IoProviderDriverRecord::ReadLifecycleSnapshot(
    const core::ControlDomainId &domain, core::TimePoint now) {
  auto lock = AcquireDelivery();
  if (domain != state_->domain.control_domain_id || now < 0)
    return core::Result<RetainedExternallyManagedDomainSnapshot>::Err(
        error("driver_domain_not_owned"));
  return state_->Snapshot();
}
core::Result<core::LifecycleDomainHealthDTO>
IoProviderDriverRecord::ReadLifecycleHealth(const core::ControlDomainId &domain,
                                            core::TimePoint now) const {
  auto lock = AcquireDelivery();
  if (domain != state_->domain.control_domain_id || now < 0)
    return core::Result<core::LifecycleDomainHealthDTO>::Err(
        error("driver_domain_not_owned"));
  const auto effective = state_->EffectiveFault();
  return core::Result<core::LifecycleDomainHealthDTO>::Ok(
      {state_->EffectiveState(effective),
       facts(state_->installed && !state_->retired, state_->enabled,
             effective.has_value(), state_->TransportReady(),
             state_->retired),
       effective ? effective : state_->TransportDiagnostic(), true});
}

RetainedExternallyManagedPrimitiveOutcome IoProviderDriverRecord::Dispatch(
    const core::BrandLifecyclePrimitiveIntentDTO &intent,
    std::stop_token cancellation) {
  using Dispatch = core::LifecyclePrimitiveDispatchStateDTO;
  using Effect = core::LifecyclePrimitiveEffectDTO;
  using Known = core::LifecyclePrimitiveKnownResultDTO;
  const auto refused = [](std::string reason) {
    return RetainedExternallyManagedPrimitiveOutcome{
        Dispatch::kNotDispatched, Effect::kNone, {}, error(std::move(reason))};
  };
  auto lock = AcquireDelivery();
  const auto observed = state_->Snapshot();
  const auto &occurrence = state_->occurrence;
  if (!observed.ok() || !intent.locally_valid() ||
      cancellation.stop_requested() || state_->retired ||
      intent.controllable_component_id !=
          state_->domain.provider()->provider.controllable_component_id ||
      intent.control_domain_id != occurrence.control_domain_id ||
      intent.control_domain_revision != occurrence.control_domain_revision ||
      intent.provider_instance_id != occurrence.provider_instance_id ||
      intent.provider_manifest_id != occurrence.provider_manifest_id ||
      intent.provider_manifest_revision !=
          occurrence.provider_manifest_revision ||
      intent.provider_runtime_unit_id != occurrence.provider_runtime_unit_id ||
      intent.provider_runtime_epoch != occurrence.provider_runtime_epoch ||
      intent.expected_observation_generation != state_->generation)
    return refused("driver_primitive_stale_or_cancelled");
  const auto &primitive = intent.lifecycle_primitive_id.value();
  using Lifecycle = core::ComponentLifecycleStateDTO;
  const bool prepare = primitive == "io.provider.prepare" &&
                       (state_->state == Lifecycle::kDeclared ||
                        state_->state == Lifecycle::kDisabled);
  const bool enable = primitive == "io.provider.enable" &&
                      state_->state == Lifecycle::kPrepared;
  const bool disable = primitive == "io.provider.disable" &&
                       (state_->state == Lifecycle::kPrepared ||
                        state_->state == Lifecycle::kActive ||
                        state_->state == Lifecycle::kDegraded);
  const bool recover = primitive == "io.provider.recover" &&
                       (state_->state == Lifecycle::kDegraded ||
                        state_->state == Lifecycle::kFaulted);
  if ((!prepare && !enable && !disable && !recover) || !state_->installed ||
      (!disable && !state_->TransportReady()) ||
      ((prepare || enable) && state_->fault) ||
      ((prepare || enable || recover) && state_->ProcessFault()))
    return refused("driver_primitive_precondition_changed");
  if (state_->generation == std::numeric_limits<std::uint64_t>::max())
    return refused("driver_generation_exhausted");
  if ((enable || disable) && !state_->inputs.empty() &&
      state_->input_lifecycle) {
    const auto planned =
        state_->input_lifecycle(intent.lifecycle_transaction_id, disable);
    if (!planned.ok()) {
      if (planned.error().details.contains("io_source_transition_effects")) {
        static_cast<void>(
            state_->Advance()); // Capacity was checked before the callback.
        state_->fault = planned.error();
        state_->state = Lifecycle::kFaulted;
        state_->CommitFacts();
        return {Dispatch::kDispatchedKnown, Effect::kObserved, Known::kFailed,
                planned.error()};
      }
      return {Dispatch::kNotDispatched, Effect::kNone, {}, planned.error()};
    }
  }
  if (!state_->Advance())
    return refused("driver_generation_exhausted");
  if (state_->cancel_pending)
    state_->cancel_pending();
  std::optional<core::ErrorDTO> failure;
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
          } else
            return leaf->Resume(state_->generation);
        },
        input.leaf);
    if (!accepted)
      failure = error("driver_input_gate_failed");
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
  if (failure) {
    state_->fault = failure;
    state_->state = Lifecycle::kFaulted;
    state_->CommitFacts();
    return {Dispatch::kDispatchedKnown, Effect::kObserved, Known::kFailed,
            failure};
  }
  state_->enabled = enable;
  state_->fault.reset();
  state_->state = enable    ? Lifecycle::kActive
                  : disable ? Lifecycle::kDisabled
                            : Lifecycle::kPrepared;
  state_->CommitFacts();
  return {Dispatch::kDispatchedKnown, Effect::kObserved, Known::kSucceeded, {}};
}

void IoProviderDriverRecord::SetSupervisedProcessFaultSource(
    std::function<std::optional<std::string>()> source) {
  auto lock = AcquireDelivery();
  state_->process_fault_source = std::move(source);
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
  state_->enabled = false;
  state_->state = core::ComponentLifecycleStateDTO::kDisabled;
  state_->CommitFacts();
}

} // namespace manyforge::roboplan_backend::host_detail
