#include "manyforge_component_manager/runtime_observation.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace manyforge::component_manager {
namespace {

void ValidateIdentifier(
    const std::string_view value,
    const std::string_view label,
    const bool optional = false,
    const std::size_t maximum_bytes = kMaximumIdentifierBytes) {
  if (optional && value.empty()) {
    return;
  }
  if (value.empty() || value.size() > maximum_bytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(
        std::string(label) + " must be a bounded printable ASCII identifier");
  }
}

void ValidatePositive(
    const std::uint64_t value,
    const std::string_view label) {
  if (value == 0U) {
    throw std::invalid_argument(std::string(label) + " must be positive");
  }
}

bool IsSha256Digest(const std::string_view value) noexcept {
  return value.size() == 71U && value.starts_with("sha256:") &&
         std::all_of(value.begin() + 7, value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

std::vector<std::string> NormalizeIdentifiers(
    std::vector<std::string> values,
    const std::string_view label,
    const std::size_t maximum,
    const std::size_t maximum_length = kMaximumIdentifierBytes) {
  if (values.size() > maximum) {
    throw std::invalid_argument(
        std::string(label) + " exceeds its fixed bound");
  }
  for (const auto& value : values) {
    ValidateIdentifier(value, label, false, maximum_length);
  }
  std::sort(values.begin(), values.end());
  if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
    throw std::invalid_argument(
        std::string(label) + " must contain unique values");
  }
  return values;
}

template <typename T>
void CheckedIncrement(T& value, const std::string_view label) {
  if (value == std::numeric_limits<T>::max()) {
    throw std::overflow_error(std::string(label) + " is exhausted");
  }
  ++value;
}

std::uint64_t CheckedAge(
    const std::uint64_t now,
    const std::uint64_t received,
    const std::string_view label) {
  if (received > now) {
    throw std::logic_error(std::string(label) + " is later than evaluation");
  }
  return now - received;
}

bool Includes(
    const std::vector<std::string>& haystack,
    const std::vector<std::string>& needles) {
  return std::includes(
      haystack.begin(),
      haystack.end(),
      needles.begin(),
      needles.end());
}

}  // namespace

RuntimeUnitExpectation::RuntimeUnitExpectation(
    std::string runtime_unit_id,
    const RuntimeLifecycleControl lifecycle_control,
    std::string product_identity,
    std::string configuration_identity,
    const std::uint64_t maximum_presence_age_ns,
    const std::uint64_t maximum_report_age_ns,
    const std::optional<std::uint64_t> maximum_progress_age_ns,
    std::vector<std::string> dependency_runtime_unit_ids,
    std::vector<std::string> required_runtime_role_ids,
    const ManagedCommandSurfaceState required_command_surface_state,
    std::string selected_runtime_contract_offer_digest)
    : runtime_unit_id_(std::move(runtime_unit_id)),
      lifecycle_control_(lifecycle_control),
      product_identity_(std::move(product_identity)),
      configuration_identity_(std::move(configuration_identity)),
      maximum_presence_age_ns_(maximum_presence_age_ns),
      maximum_report_age_ns_(maximum_report_age_ns),
      maximum_progress_age_ns_(maximum_progress_age_ns),
      dependency_runtime_unit_ids_(NormalizeIdentifiers(
          std::move(dependency_runtime_unit_ids),
          "dependency_runtime_unit_ids",
          kMaximumDependencies)),
      required_runtime_role_ids_(NormalizeIdentifiers(
          std::move(required_runtime_role_ids),
          "required_runtime_role_ids",
          kMaximumRuntimeRoles)),
      required_command_surface_state_(required_command_surface_state),
      selected_runtime_contract_offer_digest_(
          std::move(selected_runtime_contract_offer_digest)) {
  static_cast<void>(ToString(lifecycle_control_));
  static_cast<void>(ToString(required_command_surface_state_));
  ValidateIdentifier(runtime_unit_id_, "runtime_unit_id");
  ValidateIdentifier(product_identity_, "product_identity");
  ValidateIdentifier(configuration_identity_, "configuration_identity");
  ValidatePositive(maximum_presence_age_ns_, "maximum_presence_age_ns");
  ValidatePositive(maximum_report_age_ns_, "maximum_report_age_ns");
  if (maximum_progress_age_ns_) {
    ValidatePositive(*maximum_progress_age_ns_, "maximum_progress_age_ns");
  }
  if (!selected_runtime_contract_offer_digest_.empty() &&
      !IsSha256Digest(selected_runtime_contract_offer_digest_)) {
    ValidateIdentifier(selected_runtime_contract_offer_digest_,
                       "selected_runtime_contract_offer_digest");
    throw std::invalid_argument(
        "selected_runtime_contract_offer_digest must be a sha256 digest");
  }
  if (std::binary_search(
          dependency_runtime_unit_ids_.begin(),
          dependency_runtime_unit_ids_.end(),
          runtime_unit_id_)) {
    throw std::invalid_argument("a runtime unit cannot depend on itself");
  }
}

const std::string& RuntimeUnitExpectation::runtime_unit_id() const noexcept {
  return runtime_unit_id_;
}

RuntimeLifecycleControl RuntimeUnitExpectation::lifecycle_control()
    const noexcept {
  return lifecycle_control_;
}

const std::string& RuntimeUnitExpectation::product_identity() const noexcept {
  return product_identity_;
}

const std::string& RuntimeUnitExpectation::configuration_identity()
    const noexcept {
  return configuration_identity_;
}

std::uint64_t RuntimeUnitExpectation::maximum_presence_age_ns()
    const noexcept {
  return maximum_presence_age_ns_;
}

std::uint64_t RuntimeUnitExpectation::maximum_report_age_ns() const noexcept {
  return maximum_report_age_ns_;
}

std::optional<std::uint64_t>
RuntimeUnitExpectation::maximum_progress_age_ns() const noexcept {
  return maximum_progress_age_ns_;
}

const std::vector<std::string>&
RuntimeUnitExpectation::dependency_runtime_unit_ids() const noexcept {
  return dependency_runtime_unit_ids_;
}

const std::vector<std::string>&
RuntimeUnitExpectation::required_runtime_role_ids() const noexcept {
  return required_runtime_role_ids_;
}

ManagedCommandSurfaceState
RuntimeUnitExpectation::required_command_surface_state() const noexcept {
  return required_command_surface_state_;
}

const std::string&
RuntimeUnitExpectation::selected_runtime_contract_offer_digest()
    const noexcept {
  return selected_runtime_contract_offer_digest_;
}

ProcessPresenceObservation::ProcessPresenceObservation(
    std::string runtime_unit_id,
    std::string observer_instance_id,
    const std::uint64_t observation_sequence,
    const ProcessPresenceState state,
    std::string process_incarnation)
    : runtime_unit_id_(std::move(runtime_unit_id)),
      observer_instance_id_(std::move(observer_instance_id)),
      observation_sequence_(observation_sequence),
      state_(state),
      process_incarnation_(std::move(process_incarnation)) {
  static_cast<void>(ToString(state_));
  ValidateIdentifier(runtime_unit_id_, "runtime_unit_id");
  ValidateIdentifier(observer_instance_id_, "observer_instance_id");
  ValidatePositive(observation_sequence_, "observation_sequence");
  if (state_ == ProcessPresenceState::kPresent) {
    ValidateIdentifier(process_incarnation_, "process_incarnation");
  } else if (!process_incarnation_.empty()) {
    throw std::invalid_argument(
        "non-present process evidence cannot carry an incarnation");
  }
}

const std::string& ProcessPresenceObservation::runtime_unit_id()
    const noexcept {
  return runtime_unit_id_;
}

const std::string& ProcessPresenceObservation::observer_instance_id()
    const noexcept {
  return observer_instance_id_;
}

std::uint64_t ProcessPresenceObservation::observation_sequence()
    const noexcept {
  return observation_sequence_;
}

ProcessPresenceState ProcessPresenceObservation::state() const noexcept {
  return state_;
}

const std::string& ProcessPresenceObservation::process_incarnation()
    const noexcept {
  return process_incarnation_;
}

RuntimeUnitSourceReport::RuntimeUnitSourceReport(
    std::string runtime_unit_id,
    std::string process_incarnation,
    const std::uint64_t source_report_sequence,
    const std::uint64_t critical_progress_sequence,
    const RuntimeSourcePhase local_phase,
    const RuntimeSourceHealth source_health,
    std::string product_identity,
    std::string configuration_identity,
    std::vector<std::string> ready_runtime_role_ids,
    const ManagedCommandSurfaceState command_surface_state,
    std::vector<std::string> reason_codes,
    std::string offered_runtime_contract_offer_digest,
    const std::optional<std::uint64_t> dropped_host_notice_lines,
    const std::optional<std::uint64_t> dropped_host_notice_bytes)
    : runtime_unit_id_(std::move(runtime_unit_id)),
      process_incarnation_(std::move(process_incarnation)),
      source_report_sequence_(source_report_sequence),
      critical_progress_sequence_(critical_progress_sequence),
      local_phase_(local_phase),
      source_health_(source_health),
      product_identity_(std::move(product_identity)),
      configuration_identity_(std::move(configuration_identity)),
      ready_runtime_role_ids_(NormalizeIdentifiers(
          std::move(ready_runtime_role_ids),
          "ready_runtime_role_ids",
          kMaximumRuntimeRoles)),
      command_surface_state_(command_surface_state),
      offered_runtime_contract_offer_digest_(
          std::move(offered_runtime_contract_offer_digest)),
      reason_codes_(NormalizeIdentifiers(
          std::move(reason_codes),
          "reason_codes",
          kMaximumSourceReasonCodes)),
      dropped_host_notice_lines_(dropped_host_notice_lines),
      dropped_host_notice_bytes_(dropped_host_notice_bytes) {
  if (dropped_host_notice_lines_.has_value() !=
      dropped_host_notice_bytes_.has_value()) {
    throw std::invalid_argument(
        "host notice drop counters must appear together");
  }
  static_cast<void>(ToString(local_phase_));
  static_cast<void>(ToString(source_health_));
  static_cast<void>(ToString(command_surface_state_));
  ValidateIdentifier(runtime_unit_id_, "runtime_unit_id");
  ValidateIdentifier(process_incarnation_, "process_incarnation");
  ValidatePositive(source_report_sequence_, "source_report_sequence");
  ValidateIdentifier(product_identity_, "product_identity");
  ValidateIdentifier(configuration_identity_, "configuration_identity");
  if (!offered_runtime_contract_offer_digest_.empty() &&
      !IsSha256Digest(offered_runtime_contract_offer_digest_)) {
    ValidateIdentifier(offered_runtime_contract_offer_digest_,
                       "offered_runtime_contract_offer_digest");
    throw std::invalid_argument(
        "offered_runtime_contract_offer_digest must be a sha256 digest");
  }
}

const std::string& RuntimeUnitSourceReport::runtime_unit_id() const noexcept {
  return runtime_unit_id_;
}

const std::string& RuntimeUnitSourceReport::process_incarnation()
    const noexcept {
  return process_incarnation_;
}

std::uint64_t RuntimeUnitSourceReport::source_report_sequence()
    const noexcept {
  return source_report_sequence_;
}

std::uint64_t RuntimeUnitSourceReport::critical_progress_sequence()
    const noexcept {
  return critical_progress_sequence_;
}

RuntimeSourcePhase RuntimeUnitSourceReport::local_phase() const noexcept {
  return local_phase_;
}

RuntimeSourceHealth RuntimeUnitSourceReport::source_health() const noexcept {
  return source_health_;
}

const std::string& RuntimeUnitSourceReport::product_identity() const noexcept {
  return product_identity_;
}

const std::string& RuntimeUnitSourceReport::configuration_identity()
    const noexcept {
  return configuration_identity_;
}

const std::vector<std::string>&
RuntimeUnitSourceReport::ready_runtime_role_ids() const noexcept {
  return ready_runtime_role_ids_;
}

ManagedCommandSurfaceState RuntimeUnitSourceReport::command_surface_state()
    const noexcept {
  return command_surface_state_;
}

const std::string&
RuntimeUnitSourceReport::offered_runtime_contract_offer_digest()
    const noexcept {
  return offered_runtime_contract_offer_digest_;
}

const std::vector<std::string>& RuntimeUnitSourceReport::reason_codes()
    const noexcept {
  return reason_codes_;
}

std::optional<std::uint64_t>
RuntimeUnitSourceReport::dropped_host_notice_lines() const noexcept {
  return dropped_host_notice_lines_;
}

std::optional<std::uint64_t>
RuntimeUnitSourceReport::dropped_host_notice_bytes() const noexcept {
  return dropped_host_notice_bytes_;
}

RuntimeUnitObservation::RuntimeUnitObservation(
    std::string runtime_unit_id,
    const RuntimeLifecycleControl lifecycle_control,
    const RuntimeConvergenceState derived_state,
    const ProcessPresenceState process_presence,
    std::string process_observer_instance_id,
    const std::optional<std::uint64_t> process_observation_sequence,
    std::string runtime_epoch,
    const std::optional<std::uint64_t> presence_age_ns,
    const std::optional<std::uint64_t> report_age_ns,
    const std::optional<std::uint64_t> progress_age_ns,
    std::string observed_product_identity,
    std::string observed_configuration_identity,
    const std::optional<RuntimeSourcePhase> source_phase,
    const std::optional<RuntimeSourceHealth> source_health,
    const std::optional<std::uint64_t> source_report_sequence,
    const std::optional<std::uint64_t> critical_progress_sequence,
    std::vector<std::string> ready_runtime_role_ids,
    const std::optional<ManagedCommandSurfaceState> command_surface_state,
    std::string observed_runtime_contract_offer_digest,
    std::map<std::string, std::string> dependency_runtime_epochs,
    std::vector<std::string> source_reason_codes,
    std::vector<std::string> reason_codes,
    const std::optional<std::uint64_t> dropped_host_notice_lines,
    const std::optional<std::uint64_t> dropped_host_notice_bytes)
    : runtime_unit_id_(std::move(runtime_unit_id)),
      lifecycle_control_(lifecycle_control),
      derived_state_(derived_state),
      process_presence_(process_presence),
      process_observer_instance_id_(std::move(process_observer_instance_id)),
      process_observation_sequence_(process_observation_sequence),
      runtime_epoch_(std::move(runtime_epoch)),
      presence_age_ns_(presence_age_ns),
      report_age_ns_(report_age_ns),
      progress_age_ns_(progress_age_ns),
      observed_product_identity_(std::move(observed_product_identity)),
      observed_configuration_identity_(
          std::move(observed_configuration_identity)),
      source_phase_(source_phase),
      source_health_(source_health),
      source_report_sequence_(source_report_sequence),
      critical_progress_sequence_(critical_progress_sequence),
      ready_runtime_role_ids_(NormalizeIdentifiers(
          std::move(ready_runtime_role_ids),
          "ready_runtime_role_ids",
          kMaximumRuntimeRoles)),
      command_surface_state_(command_surface_state),
      observed_runtime_contract_offer_digest_(
          std::move(observed_runtime_contract_offer_digest)),
      dependency_runtime_epochs_(std::move(dependency_runtime_epochs)),
      source_reason_codes_(NormalizeIdentifiers(
          std::move(source_reason_codes),
          "source_reason_codes",
          kMaximumSourceReasonCodes)),
      reason_codes_(NormalizeIdentifiers(
          std::move(reason_codes),
          "reason_codes",
          kMaximumManagerReasonCodes,
          kMaximumIdentifierBytes + 64U)),
      dropped_host_notice_lines_(dropped_host_notice_lines),
      dropped_host_notice_bytes_(dropped_host_notice_bytes) {
  if (dropped_host_notice_lines_.has_value() !=
      dropped_host_notice_bytes_.has_value()) {
    throw std::invalid_argument(
        "host notice drop counters must appear together");
  }
  ValidateIdentifier(runtime_unit_id_, "runtime_unit_id");
  if (!process_observer_instance_id_.empty()) {
    ValidateIdentifier(
        process_observer_instance_id_,
        "process_observer_instance_id");
  }
  if (process_observer_instance_id_.empty() !=
      !process_observation_sequence_.has_value()) {
    throw std::invalid_argument(
        "process observer identity and sequence must appear together");
  }
  if (process_presence_ == ProcessPresenceState::kPresent) {
    ValidateIdentifier(runtime_epoch_, "runtime_epoch");
  } else if (!runtime_epoch_.empty()) {
    throw std::invalid_argument(
        "non-present process observation cannot carry an epoch");
  }
  if (!observed_product_identity_.empty()) {
    ValidateIdentifier(
        observed_product_identity_,
        "observed_product_identity");
  }
  if (!observed_configuration_identity_.empty()) {
    ValidateIdentifier(
        observed_configuration_identity_,
        "observed_configuration_identity");
  }
  if (!observed_runtime_contract_offer_digest_.empty() &&
      !IsSha256Digest(observed_runtime_contract_offer_digest_)) {
    ValidateIdentifier(observed_runtime_contract_offer_digest_,
                       "observed_runtime_contract_offer_digest");
    throw std::invalid_argument(
        "observed_runtime_contract_offer_digest must be a sha256 digest");
  }
  const bool has_report = source_report_sequence_.has_value();
  const bool report_complete =
      source_phase_.has_value() && source_health_.has_value() &&
      critical_progress_sequence_.has_value() &&
      !observed_product_identity_.empty() &&
      !observed_configuration_identity_.empty() &&
      command_surface_state_.has_value();
  if (has_report != report_complete) {
    throw std::invalid_argument("source report projection is incomplete");
  }
  if (has_report && process_presence_ != ProcessPresenceState::kPresent) {
    throw std::invalid_argument(
        "source report projection requires process presence");
  }
  if (dependency_runtime_epochs_.size() > kMaximumDependencies) {
    throw std::invalid_argument(
        "dependency_runtime_epochs exceeds its fixed bound");
  }
  for (const auto& [dependency, epoch] : dependency_runtime_epochs_) {
    ValidateIdentifier(dependency, "dependency runtime unit");
    ValidateIdentifier(epoch, "dependency runtime epoch");
  }
}

const std::string& RuntimeUnitObservation::runtime_unit_id() const noexcept {
  return runtime_unit_id_;
}

RuntimeLifecycleControl RuntimeUnitObservation::lifecycle_control()
    const noexcept {
  return lifecycle_control_;
}

RuntimeConvergenceState RuntimeUnitObservation::derived_state()
    const noexcept {
  return derived_state_;
}

ProcessPresenceState RuntimeUnitObservation::process_presence()
    const noexcept {
  return process_presence_;
}

const std::string& RuntimeUnitObservation::process_observer_instance_id()
    const noexcept {
  return process_observer_instance_id_;
}

std::optional<std::uint64_t>
RuntimeUnitObservation::process_observation_sequence() const noexcept {
  return process_observation_sequence_;
}

const std::string& RuntimeUnitObservation::runtime_epoch() const noexcept {
  return runtime_epoch_;
}

std::optional<std::uint64_t> RuntimeUnitObservation::presence_age_ns()
    const noexcept {
  return presence_age_ns_;
}

std::optional<std::uint64_t> RuntimeUnitObservation::report_age_ns()
    const noexcept {
  return report_age_ns_;
}

std::optional<std::uint64_t> RuntimeUnitObservation::progress_age_ns()
    const noexcept {
  return progress_age_ns_;
}

const std::string& RuntimeUnitObservation::observed_product_identity()
    const noexcept {
  return observed_product_identity_;
}

const std::string& RuntimeUnitObservation::observed_configuration_identity()
    const noexcept {
  return observed_configuration_identity_;
}

const std::string&
RuntimeUnitObservation::observed_runtime_contract_offer_digest()
    const noexcept {
  return observed_runtime_contract_offer_digest_;
}

std::optional<RuntimeSourcePhase> RuntimeUnitObservation::source_phase()
    const noexcept {
  return source_phase_;
}

std::optional<RuntimeSourceHealth> RuntimeUnitObservation::source_health()
    const noexcept {
  return source_health_;
}

std::optional<std::uint64_t>
RuntimeUnitObservation::source_report_sequence() const noexcept {
  return source_report_sequence_;
}

std::optional<std::uint64_t>
RuntimeUnitObservation::critical_progress_sequence() const noexcept {
  return critical_progress_sequence_;
}

const std::vector<std::string>&
RuntimeUnitObservation::ready_runtime_role_ids() const noexcept {
  return ready_runtime_role_ids_;
}

std::optional<ManagedCommandSurfaceState>
RuntimeUnitObservation::command_surface_state() const noexcept {
  return command_surface_state_;
}

const std::map<std::string, std::string>&
RuntimeUnitObservation::dependency_runtime_epochs() const noexcept {
  return dependency_runtime_epochs_;
}

const std::vector<std::string>&
RuntimeUnitObservation::source_reason_codes() const noexcept {
  return source_reason_codes_;
}

const std::vector<std::string>& RuntimeUnitObservation::reason_codes()
    const noexcept {
  return reason_codes_;
}

bool RuntimeUnitObservation::converged() const noexcept {
  return derived_state_ == RuntimeConvergenceState::kConverged;
}

std::optional<std::uint64_t>
RuntimeUnitObservation::dropped_host_notice_lines() const noexcept {
  return dropped_host_notice_lines_;
}

std::optional<std::uint64_t>
RuntimeUnitObservation::dropped_host_notice_bytes() const noexcept {
  return dropped_host_notice_bytes_;
}

RuntimeObservationSnapshot::RuntimeObservationSnapshot(
    const std::uint64_t evaluated_at_ns,
    std::vector<RuntimeUnitObservation> observations)
    : evaluated_at_ns_(evaluated_at_ns),
      observations_(std::move(observations)) {
  ValidatePositive(evaluated_at_ns_, "evaluated_at_ns");
  if (observations_.size() > kMaximumRuntimeUnits) {
    throw std::invalid_argument("observations exceed their fixed bound");
  }
  std::sort(
      observations_.begin(),
      observations_.end(),
      [](const auto& left, const auto& right) {
        return left.runtime_unit_id() < right.runtime_unit_id();
      });
  for (std::size_t index = 1; index < observations_.size(); ++index) {
    if (observations_[index - 1].runtime_unit_id() ==
        observations_[index].runtime_unit_id()) {
      throw std::invalid_argument(
          "observations must contain unique runtime units");
    }
  }
}

std::uint64_t RuntimeObservationSnapshot::evaluated_at_ns() const noexcept {
  return evaluated_at_ns_;
}

const std::vector<RuntimeUnitObservation>&
RuntimeObservationSnapshot::observations() const & noexcept {
  return observations_;
}

const RuntimeUnitObservation* RuntimeObservationSnapshot::observation(
    const std::string_view runtime_unit_id) const & noexcept {
  const auto found = std::lower_bound(
      observations_.begin(),
      observations_.end(),
      runtime_unit_id,
      [](const RuntimeUnitObservation& observation, const std::string_view id) {
        return observation.runtime_unit_id() < id;
      });
  return found != observations_.end() &&
          found->runtime_unit_id() == runtime_unit_id
      ? &*found
      : nullptr;
}

RuntimeUnitProcessBinding::RuntimeUnitProcessBinding(
    std::string runtime_unit_id,
    std::string process_boundary_id,
    const RuntimeLifecycleControl lifecycle_control,
    std::string process_observation_handle_id,
    std::string status_source_id,
    std::string progress_source_id,
    std::string incarnation_source_id,
    std::string managed_lifecycle_handle_id)
    : runtime_unit_id_(std::move(runtime_unit_id)),
      process_boundary_id_(std::move(process_boundary_id)),
      lifecycle_control_(lifecycle_control),
      process_observation_handle_id_(
          std::move(process_observation_handle_id)),
      status_source_id_(std::move(status_source_id)),
      progress_source_id_(std::move(progress_source_id)),
      incarnation_source_id_(std::move(incarnation_source_id)),
      managed_lifecycle_handle_id_(
          std::move(managed_lifecycle_handle_id)) {
  static_cast<void>(ToString(lifecycle_control_));
  ValidateIdentifier(runtime_unit_id_, "runtime_unit_id");
  ValidateIdentifier(process_boundary_id_, "process_boundary_id");
  ValidateIdentifier(
      process_observation_handle_id_,
      "process_observation_handle_id");
  ValidateIdentifier(status_source_id_, "status_source_id");
  ValidateIdentifier(progress_source_id_, "progress_source_id");
  ValidateIdentifier(incarnation_source_id_, "incarnation_source_id");
  if (lifecycle_control_ == RuntimeLifecycleControl::kManagerManaged) {
    ValidateIdentifier(
        managed_lifecycle_handle_id_,
        "managed_lifecycle_handle_id");
  } else if (!managed_lifecycle_handle_id_.empty()) {
    throw std::invalid_argument(
        "externally managed bindings cannot expose a Manager lifecycle handle");
  }
}

const std::string& RuntimeUnitProcessBinding::runtime_unit_id()
    const noexcept {
  return runtime_unit_id_;
}

const std::string& RuntimeUnitProcessBinding::process_boundary_id()
    const noexcept {
  return process_boundary_id_;
}

RuntimeLifecycleControl RuntimeUnitProcessBinding::lifecycle_control()
    const noexcept {
  return lifecycle_control_;
}

const std::string&
RuntimeUnitProcessBinding::process_observation_handle_id() const noexcept {
  return process_observation_handle_id_;
}

const std::string& RuntimeUnitProcessBinding::status_source_id()
    const noexcept {
  return status_source_id_;
}

const std::string& RuntimeUnitProcessBinding::progress_source_id()
    const noexcept {
  return progress_source_id_;
}

const std::string& RuntimeUnitProcessBinding::incarnation_source_id()
    const noexcept {
  return incarnation_source_id_;
}

const std::string& RuntimeUnitProcessBinding::managed_lifecycle_handle_id()
    const noexcept {
  return managed_lifecycle_handle_id_;
}

void ValidateRuntimeUnitProcessBindings(
    const std::vector<RuntimeUnitProcessBinding>& bindings) {
  if (bindings.size() > kMaximumRuntimeUnits) {
    throw std::invalid_argument("process bindings exceed their fixed bound");
  }
  std::set<std::string> runtime_ids;
  std::map<std::string, std::string> handle_owners;
  std::map<std::string, std::vector<const RuntimeUnitProcessBinding*>>
      by_boundary;
  for (const auto& binding : bindings) {
    if (!runtime_ids.insert(binding.runtime_unit_id()).second) {
      throw std::invalid_argument(
          "runtime-unit process bindings must be unique");
    }
    const std::vector<std::string_view> handles = {
        binding.process_observation_handle_id(),
        binding.status_source_id(),
        binding.progress_source_id(),
        binding.incarnation_source_id(),
        binding.managed_lifecycle_handle_id(),
    };
    for (const auto handle : handles) {
      if (handle.empty()) {
        continue;
      }
      const auto [found, inserted] = handle_owners.emplace(
          std::string(handle),
          binding.runtime_unit_id());
      if (!inserted && found->second != binding.runtime_unit_id()) {
        throw std::invalid_argument(
            "runtime-unit process bindings lack independent concrete handles: " +
            std::string(handle));
      }
    }
    by_boundary[binding.process_boundary_id()].push_back(&binding);
  }
  for (const auto& [boundary_id, grouped] : by_boundary) {
    if (grouped.size() < 2U) {
      continue;
    }
    const auto require_unique = [&](const auto getter, const auto label) {
      std::set<std::string> values;
      for (const auto* binding : grouped) {
        values.insert((binding->*getter)());
      }
      if (values.size() != grouped.size()) {
        throw std::invalid_argument(
            "co-located runtime units lack independent " +
            std::string(label) + ": " + boundary_id);
      }
    };
    require_unique(
        &RuntimeUnitProcessBinding::process_observation_handle_id,
        "process_observation_handle_id");
    require_unique(
        &RuntimeUnitProcessBinding::status_source_id,
        "status_source_id");
    require_unique(
        &RuntimeUnitProcessBinding::progress_source_id,
        "progress_source_id");
    require_unique(
        &RuntimeUnitProcessBinding::incarnation_source_id,
        "incarnation_source_id");
    std::set<std::string> managed_handles;
    std::size_t managed_count = 0;
    for (const auto* binding : grouped) {
      if (binding->lifecycle_control() ==
          RuntimeLifecycleControl::kManagerManaged) {
        ++managed_count;
        managed_handles.insert(binding->managed_lifecycle_handle_id());
      }
    }
    if (managed_handles.size() != managed_count) {
      throw std::invalid_argument(
          "co-located managed runtime units lack independent lifecycle "
          "handles: " +
          boundary_id);
    }
  }
}

class RuntimeObservationReducer::Impl final {
 public:
  struct AcceptedPresence final {
    ProcessPresenceObservation sample;
    std::uint64_t received_at_ns;
    std::uint64_t accepted_event_generation;
  };

  struct AcceptedReport final {
    RuntimeUnitSourceReport sample;
    std::uint64_t report_received_at_ns;
    std::uint64_t progress_received_at_ns;
    std::uint64_t accepted_event_generation;
  };

  struct DependencyJoin final {
    std::string process_incarnation;
    std::uint64_t source_report_sequence;
    std::string product_identity;
    std::string configuration_identity;
    std::map<std::string, std::string> runtime_epochs;
  };

  Impl(Clock clock_ns, std::vector<std::string> declared_runtime_unit_ids)
      : clock_ns_(std::move(clock_ns)),
        declared_runtime_unit_ids_(NormalizeIdentifiers(
            std::move(declared_runtime_unit_ids),
            "declared_runtime_unit_ids",
            kMaximumRuntimeUnits)),
        declared_runtime_unit_id_set_(
            declared_runtime_unit_ids_.begin(),
            declared_runtime_unit_ids_.end()) {
    if (!clock_ns_) {
      throw std::invalid_argument("clock_ns must be callable");
    }
    for (const auto& runtime_unit_id : declared_runtime_unit_ids_) {
      observer_ledgers_.emplace(
          runtime_unit_id,
          std::map<std::string, AcceptedPresence>{});
      retired_observer_ids_.emplace(
          runtime_unit_id,
          std::set<std::string>{});
      known_incarnations_.emplace(
          runtime_unit_id,
          std::set<std::string>{});
      retired_incarnations_.emplace(
          runtime_unit_id,
          std::set<std::string>{});
      reports_.emplace(
          runtime_unit_id,
          std::map<std::string, AcceptedReport>{});
    }
  }

  [[nodiscard]] bool Reject(const std::string& reason) {
    if (!rejection_reasons_.contains(reason) &&
        rejection_reasons_.size() >= kMaximumRejectionReasons) {
      throw std::overflow_error("rejection reason cardinality is exhausted");
    }
    CheckedIncrement(rejected_total_, "rejected_total");
    CheckedIncrement(rejection_reasons_[reason], "rejection reason counter");
    return false;
  }

  [[nodiscard]] std::uint64_t ReceiverNow() {
    const auto now = clock_ns_();
    ValidatePositive(now, "receiver clock");
    if (last_clock_ns_ && now < *last_clock_ns_) {
      throw std::runtime_error("receiver monotonic clock regressed");
    }
    last_clock_ns_ = now;
    return now;
  }

  [[nodiscard]] std::uint64_t NextAcceptedEventGeneration() {
    CheckedIncrement(
        accepted_event_generation_,
        "accepted event generation");
    return accepted_event_generation_;
  }

  [[nodiscard]] bool RegisterIncarnation(
      const std::string& runtime_unit_id,
      const std::string& process_incarnation) {
    auto& known = known_incarnations_.at(runtime_unit_id);
    if (known.contains(process_incarnation)) {
      return true;
    }
    if (known.size() >= kMaximumIncarnationsPerRuntimeUnit) {
      return Reject("process_incarnation_limit_exceeded");
    }
    known.insert(process_incarnation);
    return true;
  }

  [[nodiscard]] bool AcceptProcessPresence(
      const ProcessPresenceObservation& sample,
      const std::uint64_t received_at_ns,
      const bool replacing_observer) {
    const auto& runtime_unit_id = sample.runtime_unit_id();
    auto& observer_ledger = observer_ledgers_.at(runtime_unit_id);
    const auto current_for_observer =
        observer_ledger.find(sample.observer_instance_id());
    if (current_for_observer != observer_ledger.end()) {
      const auto current_sequence =
          current_for_observer->second.sample.observation_sequence();
      if (sample.observation_sequence() < current_sequence) {
        return Reject("process_observation_sequence_regressed");
      }
      if (sample.observation_sequence() == current_sequence) {
        if (sample == current_for_observer->second.sample) {
          CheckedIncrement(duplicate_total_, "duplicate_total");
          return false;
        }
        return Reject("process_observation_sequence_conflict");
      }
    }

    const auto current_epoch_it =
        last_present_incarnation_.find(runtime_unit_id);
    const std::string current_epoch =
        current_epoch_it == last_present_incarnation_.end()
        ? std::string{}
        : current_epoch_it->second;
    const std::string candidate_epoch =
        sample.state() == ProcessPresenceState::kPresent
        ? sample.process_incarnation()
        : std::string{};
    if (!candidate_epoch.empty() &&
        retired_incarnations_.at(runtime_unit_id).contains(candidate_epoch) &&
        candidate_epoch != current_epoch) {
      return Reject("process_incarnation_reused");
    }
    if (sample.state() == ProcessPresenceState::kPresent &&
        !RegisterIncarnation(runtime_unit_id, candidate_epoch)) {
      return false;
    }

    const auto accepted_generation = NextAcceptedEventGeneration();
    if (sample.state() == ProcessPresenceState::kPresent) {
      if (!current_epoch.empty() && candidate_epoch != current_epoch) {
        retired_incarnations_.at(runtime_unit_id).insert(current_epoch);
        reports_.at(runtime_unit_id).erase(current_epoch);
      }
      if (candidate_epoch != current_epoch) {
        epoch_change_event_generations_[runtime_unit_id] =
            accepted_generation;
      }
      last_present_incarnation_[runtime_unit_id] = candidate_epoch;
    } else if (
        sample.state() == ProcessPresenceState::kAbsent &&
        !current_epoch.empty()) {
      retired_incarnations_.at(runtime_unit_id).insert(current_epoch);
      reports_.at(runtime_unit_id).erase(current_epoch);
      last_present_incarnation_.erase(runtime_unit_id);
    }

    AcceptedPresence accepted{
        sample,
        received_at_ns,
        accepted_generation,
    };
    if (replacing_observer) {
      const auto previous = active_observer_ids_.at(runtime_unit_id);
      retired_observer_ids_.at(runtime_unit_id).insert(previous);
    }
    observer_ledger.insert_or_assign(
        sample.observer_instance_id(),
        accepted);
    active_observer_ids_[runtime_unit_id] = sample.observer_instance_id();
    presence_.insert_or_assign(runtime_unit_id, std::move(accepted));
    CheckedIncrement(
        accepted_presence_total_,
        "accepted_presence_total");
    return true;
  }

  [[nodiscard]] bool ObserveProcess(
      const ProcessPresenceObservation& sample) {
    if (!declared_runtime_unit_id_set_.contains(sample.runtime_unit_id())) {
      return Reject("process_presence_runtime_unit_undeclared");
    }
    const auto active = active_observer_ids_.find(sample.runtime_unit_id());
    if (active != active_observer_ids_.end() &&
        sample.observer_instance_id() != active->second) {
      if (retired_observer_ids_.at(sample.runtime_unit_id())
              .contains(sample.observer_instance_id())) {
        return Reject("process_observer_session_retired");
      }
      return Reject("process_observer_session_not_active");
    }
    return AcceptProcessPresence(sample, ReceiverNow(), false);
  }

  [[nodiscard]] bool ReplaceProcessObserver(
      const ProcessPresenceObservation& sample) {
    const auto& runtime_unit_id = sample.runtime_unit_id();
    if (!declared_runtime_unit_id_set_.contains(runtime_unit_id)) {
      return Reject("process_presence_runtime_unit_undeclared");
    }
    const auto active = active_observer_ids_.find(runtime_unit_id);
    if (active == active_observer_ids_.end() ||
        active->second == sample.observer_instance_id()) {
      return ObserveProcess(sample);
    }
    if (retired_observer_ids_.at(runtime_unit_id)
            .contains(sample.observer_instance_id()) ||
        observer_ledgers_.at(runtime_unit_id)
            .contains(sample.observer_instance_id())) {
      return Reject("process_observer_session_reused");
    }
    if (observer_ledgers_.at(runtime_unit_id).size() >=
        kMaximumObserverSessionsPerRuntimeUnit) {
      return Reject("process_observer_session_limit_exceeded");
    }
    return AcceptProcessPresence(sample, ReceiverNow(), true);
  }

  [[nodiscard]] bool IngestSourceReport(
      const RuntimeUnitSourceReport& report) {
    const auto& runtime_unit_id = report.runtime_unit_id();
    if (!declared_runtime_unit_id_set_.contains(runtime_unit_id)) {
      return Reject("source_report_runtime_unit_undeclared");
    }
    const auto presence = presence_.find(runtime_unit_id);
    if (presence == presence_.end() ||
        presence->second.sample.state() != ProcessPresenceState::kPresent ||
        presence->second.sample.process_incarnation() !=
            report.process_incarnation()) {
      return Reject("source_report_without_matching_process_presence");
    }
    if (retired_incarnations_.at(runtime_unit_id)
            .contains(report.process_incarnation())) {
      return Reject("source_report_retired_incarnation");
    }
    if (!RegisterIncarnation(runtime_unit_id, report.process_incarnation())) {
      return false;
    }

    auto& by_incarnation = reports_.at(runtime_unit_id);
    const auto current = by_incarnation.find(report.process_incarnation());
    if (current != by_incarnation.end()) {
      if (report.source_report_sequence() <
          current->second.sample.source_report_sequence()) {
        return Reject("source_report_sequence_regressed");
      }
      if (report.source_report_sequence() ==
          current->second.sample.source_report_sequence()) {
        if (report == current->second.sample) {
          CheckedIncrement(duplicate_total_, "duplicate_total");
          return false;
        }
        return Reject("source_report_sequence_conflict");
      }
      if (report.critical_progress_sequence() <
          current->second.sample.critical_progress_sequence()) {
        return Reject("critical_progress_sequence_regressed");
      }
    }
    const auto received_at_ns = ReceiverNow();
    const auto progress_received_at_ns =
        current == by_incarnation.end() ||
            report.critical_progress_sequence() >
                current->second.sample.critical_progress_sequence()
        ? received_at_ns
        : current->second.progress_received_at_ns;
    by_incarnation.insert_or_assign(
        report.process_incarnation(),
        AcceptedReport{
            report,
            received_at_ns,
            progress_received_at_ns,
            NextAcceptedEventGeneration(),
        });
    CheckedIncrement(
        accepted_source_report_total_,
        "accepted_source_report_total");
    return true;
  }

  [[nodiscard]] std::vector<const RuntimeUnitExpectation*>
  ValidateAndOrderExpectations(
      const std::vector<RuntimeUnitExpectation>& expectations) const {
    if (expectations.size() > kMaximumRuntimeUnits) {
      throw std::invalid_argument(
          "runtime expectations exceed their fixed bound");
    }
    std::map<std::string, const RuntimeUnitExpectation*> by_id;
    for (const auto& expectation : expectations) {
      if (!declared_runtime_unit_id_set_.contains(
              expectation.runtime_unit_id())) {
        throw std::invalid_argument(
            "runtime expectation references an undeclared runtime unit");
      }
      if (!by_id.emplace(expectation.runtime_unit_id(), &expectation).second) {
        throw std::invalid_argument(
            "runtime expectations must be unique");
      }
    }
    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::set<std::string>> dependants;
    for (const auto& [runtime_unit_id, expectation] : by_id) {
      indegree[runtime_unit_id] = 0U;
      dependants[runtime_unit_id] = {};
      for (const auto& dependency :
           expectation->dependency_runtime_unit_ids()) {
        if (!by_id.contains(dependency)) {
          throw std::invalid_argument(
              "runtime expectations contain unknown dependencies: " +
              dependency);
        }
      }
    }
    for (const auto& [runtime_unit_id, expectation] : by_id) {
      for (const auto& dependency :
           expectation->dependency_runtime_unit_ids()) {
        ++indegree[runtime_unit_id];
        dependants[dependency].insert(runtime_unit_id);
      }
    }
    std::set<std::string> ready;
    for (const auto& [runtime_unit_id, degree] : indegree) {
      if (degree == 0U) {
        ready.insert(runtime_unit_id);
      }
    }
    std::vector<const RuntimeUnitExpectation*> ordered;
    ordered.reserve(expectations.size());
    while (!ready.empty()) {
      const auto runtime_unit_id = *ready.begin();
      ready.erase(ready.begin());
      ordered.push_back(by_id.at(runtime_unit_id));
      for (const auto& dependant : dependants.at(runtime_unit_id)) {
        auto& degree = indegree.at(dependant);
        --degree;
        if (degree == 0U) {
          ready.insert(dependant);
        }
      }
    }
    if (ordered.size() != by_id.size()) {
      throw std::invalid_argument(
          "runtime expectation dependencies contain a cycle");
    }
    return ordered;
  }

  [[nodiscard]] RuntimeUnitObservation MakeObservation(
      const RuntimeUnitExpectation& expectation,
      const RuntimeConvergenceState state,
      const ProcessPresenceState process_presence,
      std::string process_observer_instance_id,
      std::optional<std::uint64_t> process_observation_sequence,
      std::string runtime_epoch,
      std::optional<std::uint64_t> presence_age_ns,
      std::optional<std::uint64_t> report_age_ns,
      std::optional<std::uint64_t> progress_age_ns,
      const RuntimeUnitSourceReport* report,
      std::map<std::string, std::string> dependency_epochs,
      std::set<std::string> reasons) const {
    if (reasons.size() > kMaximumManagerReasonCodes) {
      throw std::runtime_error("generated too many Manager reason codes");
    }
    std::vector<std::string> reason_codes(reasons.begin(), reasons.end());
    return RuntimeUnitObservation(
        expectation.runtime_unit_id(),
        expectation.lifecycle_control(),
        state,
        process_presence,
        std::move(process_observer_instance_id),
        process_observation_sequence,
        std::move(runtime_epoch),
        presence_age_ns,
        report_age_ns,
        progress_age_ns,
        report == nullptr ? std::string{} : report->product_identity(),
        report == nullptr ? std::string{} : report->configuration_identity(),
        report == nullptr
            ? std::optional<RuntimeSourcePhase>{}
            : report->local_phase(),
        report == nullptr
            ? std::optional<RuntimeSourceHealth>{}
            : report->source_health(),
        report == nullptr
            ? std::optional<std::uint64_t>{}
            : report->source_report_sequence(),
        report == nullptr
            ? std::optional<std::uint64_t>{}
            : report->critical_progress_sequence(),
        report == nullptr
            ? std::vector<std::string>{}
            : report->ready_runtime_role_ids(),
        report == nullptr
            ? std::optional<ManagedCommandSurfaceState>{}
            : report->command_surface_state(),
        report == nullptr
            ? std::string{}
            : report->offered_runtime_contract_offer_digest(),
        std::move(dependency_epochs),
        report == nullptr
            ? std::vector<std::string>{}
            : report->reason_codes(),
        std::move(reason_codes),
        report == nullptr ? std::optional<std::uint64_t>{}
                          : report->dropped_host_notice_lines(),
        report == nullptr ? std::optional<std::uint64_t>{}
                          : report->dropped_host_notice_bytes());
  }

  [[nodiscard]] RuntimeUnitObservation EvaluateOne(
      const RuntimeUnitExpectation& expectation,
      const std::map<std::string, RuntimeUnitObservation>& derived,
      const std::uint64_t now_ns) {
    const auto& runtime_unit_id = expectation.runtime_unit_id();
    const auto presence = presence_.find(runtime_unit_id);
    if (presence == presence_.end()) {
      return MakeObservation(
          expectation,
          RuntimeConvergenceState::kLost,
          ProcessPresenceState::kUnknown,
          {},
          std::nullopt,
          {},
          std::nullopt,
          std::nullopt,
          std::nullopt,
          nullptr,
          {},
          {"process_presence_missing"});
    }

    std::set<std::string> reasons;
    const auto presence_age =
        CheckedAge(now_ns, presence->second.received_at_ns, "presence time");
    if (presence_age > expectation.maximum_presence_age_ns()) {
      reasons.insert("process_presence_stale");
    }
    const auto process_presence = presence->second.sample.state();
    std::string runtime_epoch;
    if (process_presence == ProcessPresenceState::kUnknown) {
      reasons.insert("process_presence_unknown");
    } else if (process_presence == ProcessPresenceState::kAbsent) {
      reasons.insert("process_absent");
    } else {
      runtime_epoch = presence->second.sample.process_incarnation();
    }
    if (!reasons.empty()) {
      return MakeObservation(
          expectation,
          RuntimeConvergenceState::kLost,
          process_presence,
          presence->second.sample.observer_instance_id(),
          presence->second.sample.observation_sequence(),
          runtime_epoch,
          presence_age,
          std::nullopt,
          std::nullopt,
          nullptr,
          {},
          std::move(reasons));
    }

    const auto report =
        reports_.at(runtime_unit_id).find(runtime_epoch);
    if (report == reports_.at(runtime_unit_id).end()) {
      return MakeObservation(
          expectation,
          last_converged_epoch_.contains(runtime_unit_id)
              ? RuntimeConvergenceState::kRecovering
              : RuntimeConvergenceState::kNonconverged,
          process_presence,
          presence->second.sample.observer_instance_id(),
          presence->second.sample.observation_sequence(),
          runtime_epoch,
          presence_age,
          std::nullopt,
          std::nullopt,
          nullptr,
          {},
          {"source_report_missing"});
    }

    const auto report_age = CheckedAge(
        now_ns,
        report->second.report_received_at_ns,
        "report time");
    const auto progress_age = CheckedAge(
        now_ns,
        report->second.progress_received_at_ns,
        "progress time");
    if (report_age > expectation.maximum_report_age_ns()) {
      reasons.insert("source_report_stale");
    }
    if (expectation.maximum_progress_age_ns() &&
        progress_age > *expectation.maximum_progress_age_ns()) {
      reasons.insert("critical_progress_stale");
    }
    if (!reasons.empty()) {
      return MakeObservation(
          expectation,
          RuntimeConvergenceState::kLost,
          process_presence,
          presence->second.sample.observer_instance_id(),
          presence->second.sample.observation_sequence(),
          runtime_epoch,
          presence_age,
          report_age,
          progress_age,
          &report->second.sample,
          {},
          std::move(reasons));
    }

    const auto& source = report->second.sample;
    if (source.product_identity() != expectation.product_identity()) {
      reasons.insert("runtime_product_identity_mismatch");
    }
    if (source.configuration_identity() !=
        expectation.configuration_identity()) {
      reasons.insert("runtime_configuration_identity_mismatch");
    }
    if (!expectation.selected_runtime_contract_offer_digest().empty() &&
        source.offered_runtime_contract_offer_digest() !=
            expectation.selected_runtime_contract_offer_digest()) {
      reasons.insert("runtime_contract_offer_digest_mismatch");
    }
    if (source.local_phase() != RuntimeSourcePhase::kServing) {
      reasons.insert(
          "runtime_source_phase_" + std::string(ToString(source.local_phase())));
    }
    if (source.source_health() != RuntimeSourceHealth::kHealthy) {
      reasons.insert(
          "runtime_source_health_" +
          std::string(ToString(source.source_health())));
    }
    if (!Includes(
            source.ready_runtime_role_ids(),
            expectation.required_runtime_role_ids())) {
      reasons.insert("runtime_roles_not_ready");
    }
    if (source.command_surface_state() !=
        expectation.required_command_surface_state()) {
      reasons.insert("runtime_command_surface_state_mismatch");
    }

    std::map<std::string, std::string> dependency_epochs;
    for (const auto& dependency_id :
         expectation.dependency_runtime_unit_ids()) {
      const auto& dependency = derived.at(dependency_id);
      if (!dependency.converged()) {
        reasons.insert(
            "runtime_dependency_not_converged:" + dependency_id);
      } else if (dependency.runtime_epoch().empty()) {
        reasons.insert(
            "runtime_dependency_epoch_missing:" + dependency_id);
      } else {
        dependency_epochs.emplace(
            dependency_id,
            dependency.runtime_epoch());
        const auto epoch_change =
            epoch_change_event_generations_.find(dependency_id);
        if (epoch_change == epoch_change_event_generations_.end() ||
            report->second.accepted_event_generation <=
                epoch_change->second) {
          reasons.insert("runtime_dependency_epoch_changed");
        }
      }
    }

    if (reasons.empty()) {
      const auto join = dependency_joins_.find(runtime_unit_id);
      const bool same_report =
          join != dependency_joins_.end() &&
          join->second.process_incarnation == source.process_incarnation() &&
          join->second.source_report_sequence ==
              source.source_report_sequence() &&
          join->second.product_identity == source.product_identity() &&
          join->second.configuration_identity ==
              source.configuration_identity();
      if (same_report &&
          join->second.runtime_epochs != dependency_epochs) {
        reasons.insert("runtime_dependency_epoch_changed");
      } else if (!same_report) {
        dependency_joins_[runtime_unit_id] = DependencyJoin{
            source.process_incarnation(),
            source.source_report_sequence(),
            source.product_identity(),
            source.configuration_identity(),
            dependency_epochs,
        };
      }
    }

    RuntimeConvergenceState state;
    if (reasons.empty()) {
      state = RuntimeConvergenceState::kConverged;
      last_converged_epoch_[runtime_unit_id] = runtime_epoch;
    } else if (reasons.contains("runtime_dependency_epoch_changed")) {
      state = RuntimeConvergenceState::kRecovering;
    } else {
      state = RuntimeConvergenceState::kNonconverged;
    }
    return MakeObservation(
        expectation,
        state,
        process_presence,
        presence->second.sample.observer_instance_id(),
        presence->second.sample.observation_sequence(),
        runtime_epoch,
        presence_age,
        report_age,
        progress_age,
        &source,
        std::move(dependency_epochs),
        std::move(reasons));
  }

  [[nodiscard]] RuntimeObservationSnapshot Evaluate(
      const std::vector<RuntimeUnitExpectation>& expectations) {
    const auto now = ReceiverNow();
    const auto ordered = ValidateAndOrderExpectations(expectations);
    std::map<std::string, RuntimeUnitObservation> derived;
    for (const auto* expectation : ordered) {
      derived.emplace(
          expectation->runtime_unit_id(),
          EvaluateOne(*expectation, derived, now));
    }
    std::vector<RuntimeUnitObservation> observations;
    observations.reserve(ordered.size());
    for (const auto* expectation : ordered) {
      observations.push_back(derived.at(expectation->runtime_unit_id()));
    }
    return RuntimeObservationSnapshot(now, std::move(observations));
  }

  Clock clock_ns_;
  std::optional<std::uint64_t> last_clock_ns_;
  std::vector<std::string> declared_runtime_unit_ids_;
  std::set<std::string> declared_runtime_unit_id_set_;
  std::map<std::string, AcceptedPresence> presence_;
  std::map<std::string, std::string> active_observer_ids_;
  std::map<std::string, std::map<std::string, AcceptedPresence>>
      observer_ledgers_;
  std::map<std::string, std::set<std::string>> retired_observer_ids_;
  std::map<std::string, std::set<std::string>> known_incarnations_;
  std::map<std::string, std::set<std::string>> retired_incarnations_;
  std::map<std::string, std::string> last_present_incarnation_;
  std::map<std::string, std::uint64_t> epoch_change_event_generations_;
  std::map<std::string, std::map<std::string, AcceptedReport>> reports_;
  std::map<std::string, DependencyJoin> dependency_joins_;
  std::map<std::string, std::string> last_converged_epoch_;
  std::uint64_t accepted_presence_total_{};
  std::uint64_t accepted_source_report_total_{};
  std::uint64_t duplicate_total_{};
  std::uint64_t rejected_total_{};
  std::map<std::string, std::uint64_t> rejection_reasons_;
  std::uint64_t accepted_event_generation_{};
};

RuntimeObservationReducer::RuntimeObservationReducer(
    Clock clock_ns,
    std::vector<std::string> declared_runtime_unit_ids)
    : impl_(std::make_unique<Impl>(
          std::move(clock_ns),
          std::move(declared_runtime_unit_ids))) {}

RuntimeObservationReducer::~RuntimeObservationReducer() = default;
RuntimeObservationReducer::RuntimeObservationReducer(
    RuntimeObservationReducer&&) noexcept = default;
RuntimeObservationReducer& RuntimeObservationReducer::operator=(
    RuntimeObservationReducer&&) noexcept = default;

const std::vector<std::string>&
RuntimeObservationReducer::declared_runtime_unit_ids() const noexcept {
  return impl_->declared_runtime_unit_ids_;
}

RuntimeObservationReducerMetrics RuntimeObservationReducer::metrics() const {
  return RuntimeObservationReducerMetrics{
      impl_->accepted_presence_total_,
      impl_->accepted_source_report_total_,
      impl_->duplicate_total_,
      impl_->rejected_total_,
      impl_->rejection_reasons_,
  };
}

bool RuntimeObservationReducer::ObserveProcess(
    const ProcessPresenceObservation& sample) {
  return impl_->ObserveProcess(sample);
}

bool RuntimeObservationReducer::ReplaceProcessObserver(
    const ProcessPresenceObservation& sample) {
  return impl_->ReplaceProcessObserver(sample);
}

bool RuntimeObservationReducer::IngestSourceReport(
    const RuntimeUnitSourceReport& report) {
  return impl_->IngestSourceReport(report);
}

RuntimeObservationSnapshot RuntimeObservationReducer::Evaluate(
    const std::vector<RuntimeUnitExpectation>& expectations) {
  return impl_->Evaluate(expectations);
}

std::string_view ToString(const RuntimeLifecycleControl value) {
  switch (value) {
    case RuntimeLifecycleControl::kManagerManaged:
      return "manager_managed";
    case RuntimeLifecycleControl::kExternallyManaged:
      return "externally_managed";
  }
  throw std::invalid_argument("invalid RuntimeLifecycleControl");
}

std::string_view ToString(const ProcessPresenceState value) {
  switch (value) {
    case ProcessPresenceState::kUnknown:
      return "unknown";
    case ProcessPresenceState::kAbsent:
      return "absent";
    case ProcessPresenceState::kPresent:
      return "present";
  }
  throw std::invalid_argument("invalid ProcessPresenceState");
}

std::string_view ToString(const RuntimeSourcePhase value) {
  switch (value) {
    case RuntimeSourcePhase::kStarting:
      return "starting";
    case RuntimeSourcePhase::kServing:
      return "serving";
    case RuntimeSourcePhase::kStopping:
      return "stopping";
    case RuntimeSourcePhase::kStopped:
      return "stopped";
    case RuntimeSourcePhase::kFaulted:
      return "faulted";
  }
  throw std::invalid_argument("invalid RuntimeSourcePhase");
}

std::string_view ToString(const RuntimeSourceHealth value) {
  switch (value) {
    case RuntimeSourceHealth::kHealthy:
      return "healthy";
    case RuntimeSourceHealth::kDegraded:
      return "degraded";
    case RuntimeSourceHealth::kFaulted:
      return "faulted";
  }
  throw std::invalid_argument("invalid RuntimeSourceHealth");
}

std::string_view ToString(const ManagedCommandSurfaceState value) {
  switch (value) {
    case ManagedCommandSurfaceState::kClosedNoActiveEffect:
      return "closed_no_active_effect";
    case ManagedCommandSurfaceState::kOpen:
      return "open";
    case ManagedCommandSurfaceState::kUnavailable:
      return "unavailable";
  }
  throw std::invalid_argument("invalid ManagedCommandSurfaceState");
}

std::string_view ToString(const RuntimeConvergenceState value) {
  switch (value) {
    case RuntimeConvergenceState::kConverged:
      return "converged";
    case RuntimeConvergenceState::kNonconverged:
      return "nonconverged";
    case RuntimeConvergenceState::kLost:
      return "lost";
    case RuntimeConvergenceState::kRecovering:
      return "recovering";
  }
  throw std::invalid_argument("invalid RuntimeConvergenceState");
}

}  // namespace manyforge::component_manager
