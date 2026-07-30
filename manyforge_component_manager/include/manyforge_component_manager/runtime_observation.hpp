#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::size_t kMaximumIdentifierBytes = 256;
inline constexpr std::size_t kMaximumRuntimeUnits = 64;
inline constexpr std::size_t kMaximumDependencies = 32;
inline constexpr std::size_t kMaximumRuntimeRoles = 256;
inline constexpr std::size_t kMaximumSourceReasonCodes = 32;
inline constexpr std::size_t kMaximumManagerReasonCodes = 64;
inline constexpr std::size_t kMaximumRejectionReasons = 32;
inline constexpr std::size_t kMaximumObserverSessionsPerRuntimeUnit = 64;
inline constexpr std::size_t kMaximumIncarnationsPerRuntimeUnit = 64;

enum class RuntimeLifecycleControl {
  kManagerManaged,
  kExternallyManaged,
};

enum class ProcessPresenceState {
  kUnknown,
  kAbsent,
  kPresent,
};

enum class RuntimeSourcePhase {
  kStarting,
  kServing,
  kStopping,
  kStopped,
  kFaulted,
};

enum class RuntimeSourceHealth {
  kHealthy,
  kDegraded,
  kFaulted,
};

enum class ManagedCommandSurfaceState {
  kClosedNoActiveEffect,
  kOpen,
  kUnavailable,
};

enum class RuntimeConvergenceState {
  kConverged,
  kNonconverged,
  kLost,
  kRecovering,
};

class RuntimeUnitExpectation final {
 public:
  RuntimeUnitExpectation(
      std::string runtime_unit_id,
      RuntimeLifecycleControl lifecycle_control,
      std::string product_identity,
      std::string configuration_identity,
      std::uint64_t maximum_presence_age_ns,
      std::uint64_t maximum_report_age_ns,
      std::optional<std::uint64_t> maximum_progress_age_ns = std::nullopt,
      std::vector<std::string> dependency_runtime_unit_ids = {},
      std::vector<std::string> required_runtime_role_ids = {},
      ManagedCommandSurfaceState required_command_surface_state =
          ManagedCommandSurfaceState::kClosedNoActiveEffect);

  [[nodiscard]] const std::string& runtime_unit_id() const noexcept;
  [[nodiscard]] RuntimeLifecycleControl lifecycle_control() const noexcept;
  [[nodiscard]] const std::string& product_identity() const noexcept;
  [[nodiscard]] const std::string& configuration_identity() const noexcept;
  [[nodiscard]] std::uint64_t maximum_presence_age_ns() const noexcept;
  [[nodiscard]] std::uint64_t maximum_report_age_ns() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> maximum_progress_age_ns()
      const noexcept;
  [[nodiscard]] const std::vector<std::string>& dependency_runtime_unit_ids()
      const noexcept;
  [[nodiscard]] const std::vector<std::string>& required_runtime_role_ids()
      const noexcept;
  [[nodiscard]] ManagedCommandSurfaceState required_command_surface_state()
      const noexcept;

 private:
  std::string runtime_unit_id_;
  RuntimeLifecycleControl lifecycle_control_;
  std::string product_identity_;
  std::string configuration_identity_;
  std::uint64_t maximum_presence_age_ns_;
  std::uint64_t maximum_report_age_ns_;
  std::optional<std::uint64_t> maximum_progress_age_ns_;
  std::vector<std::string> dependency_runtime_unit_ids_;
  std::vector<std::string> required_runtime_role_ids_;
  ManagedCommandSurfaceState required_command_surface_state_;
};

class ProcessPresenceObservation final {
 public:
  ProcessPresenceObservation(
      std::string runtime_unit_id,
      std::string observer_instance_id,
      std::uint64_t observation_sequence,
      ProcessPresenceState state,
      std::string process_incarnation = {});

  [[nodiscard]] const std::string& runtime_unit_id() const noexcept;
  [[nodiscard]] const std::string& observer_instance_id() const noexcept;
  [[nodiscard]] std::uint64_t observation_sequence() const noexcept;
  [[nodiscard]] ProcessPresenceState state() const noexcept;
  [[nodiscard]] const std::string& process_incarnation() const noexcept;

  bool operator==(const ProcessPresenceObservation&) const = default;

 private:
  std::string runtime_unit_id_;
  std::string observer_instance_id_;
  std::uint64_t observation_sequence_;
  ProcessPresenceState state_;
  std::string process_incarnation_;
};

class RuntimeUnitSourceReport final {
 public:
  RuntimeUnitSourceReport(
      std::string runtime_unit_id,
      std::string process_incarnation,
      std::uint64_t source_report_sequence,
      std::uint64_t critical_progress_sequence,
      RuntimeSourcePhase local_phase,
      RuntimeSourceHealth source_health,
      std::string product_identity,
      std::string configuration_identity,
      std::vector<std::string> ready_runtime_role_ids,
      ManagedCommandSurfaceState command_surface_state,
      std::vector<std::string> reason_codes = {});

  [[nodiscard]] const std::string& runtime_unit_id() const noexcept;
  [[nodiscard]] const std::string& process_incarnation() const noexcept;
  [[nodiscard]] std::uint64_t source_report_sequence() const noexcept;
  [[nodiscard]] std::uint64_t critical_progress_sequence() const noexcept;
  [[nodiscard]] RuntimeSourcePhase local_phase() const noexcept;
  [[nodiscard]] RuntimeSourceHealth source_health() const noexcept;
  [[nodiscard]] const std::string& product_identity() const noexcept;
  [[nodiscard]] const std::string& configuration_identity() const noexcept;
  [[nodiscard]] const std::vector<std::string>& ready_runtime_role_ids()
      const noexcept;
  [[nodiscard]] ManagedCommandSurfaceState command_surface_state()
      const noexcept;
  [[nodiscard]] const std::vector<std::string>& reason_codes() const noexcept;

  bool operator==(const RuntimeUnitSourceReport&) const = default;

 private:
  std::string runtime_unit_id_;
  std::string process_incarnation_;
  std::uint64_t source_report_sequence_;
  std::uint64_t critical_progress_sequence_;
  RuntimeSourcePhase local_phase_;
  RuntimeSourceHealth source_health_;
  std::string product_identity_;
  std::string configuration_identity_;
  std::vector<std::string> ready_runtime_role_ids_;
  ManagedCommandSurfaceState command_surface_state_;
  std::vector<std::string> reason_codes_;
};

class RuntimeUnitObservation final {
 public:
  [[nodiscard]] const std::string& runtime_unit_id() const noexcept;
  [[nodiscard]] RuntimeLifecycleControl lifecycle_control() const noexcept;
  [[nodiscard]] RuntimeConvergenceState derived_state() const noexcept;
  [[nodiscard]] ProcessPresenceState process_presence() const noexcept;
  [[nodiscard]] const std::string& process_observer_instance_id()
      const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> process_observation_sequence()
      const noexcept;
  [[nodiscard]] const std::string& runtime_epoch() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> presence_age_ns() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> report_age_ns() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> progress_age_ns() const noexcept;
  [[nodiscard]] const std::string& observed_product_identity() const noexcept;
  [[nodiscard]] const std::string& observed_configuration_identity()
      const noexcept;
  [[nodiscard]] std::optional<RuntimeSourcePhase> source_phase() const noexcept;
  [[nodiscard]] std::optional<RuntimeSourceHealth> source_health()
      const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> source_report_sequence()
      const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> critical_progress_sequence()
      const noexcept;
  [[nodiscard]] const std::vector<std::string>& ready_runtime_role_ids()
      const noexcept;
  [[nodiscard]] std::optional<ManagedCommandSurfaceState>
  command_surface_state() const noexcept;
  [[nodiscard]] const std::map<std::string, std::string>&
  dependency_runtime_epochs() const noexcept;
  [[nodiscard]] const std::vector<std::string>& source_reason_codes()
      const noexcept;
  [[nodiscard]] const std::vector<std::string>& reason_codes() const noexcept;
  [[nodiscard]] bool converged() const noexcept;

 private:
  friend class RuntimeObservationReducer;

  RuntimeUnitObservation(
      std::string runtime_unit_id,
      RuntimeLifecycleControl lifecycle_control,
      RuntimeConvergenceState derived_state,
      ProcessPresenceState process_presence,
      std::string process_observer_instance_id,
      std::optional<std::uint64_t> process_observation_sequence,
      std::string runtime_epoch,
      std::optional<std::uint64_t> presence_age_ns,
      std::optional<std::uint64_t> report_age_ns,
      std::optional<std::uint64_t> progress_age_ns,
      std::string observed_product_identity,
      std::string observed_configuration_identity,
      std::optional<RuntimeSourcePhase> source_phase,
      std::optional<RuntimeSourceHealth> source_health,
      std::optional<std::uint64_t> source_report_sequence,
      std::optional<std::uint64_t> critical_progress_sequence,
      std::vector<std::string> ready_runtime_role_ids,
      std::optional<ManagedCommandSurfaceState> command_surface_state,
      std::map<std::string, std::string> dependency_runtime_epochs,
      std::vector<std::string> source_reason_codes,
      std::vector<std::string> reason_codes);

  std::string runtime_unit_id_;
  RuntimeLifecycleControl lifecycle_control_;
  RuntimeConvergenceState derived_state_;
  ProcessPresenceState process_presence_;
  std::string process_observer_instance_id_;
  std::optional<std::uint64_t> process_observation_sequence_;
  std::string runtime_epoch_;
  std::optional<std::uint64_t> presence_age_ns_;
  std::optional<std::uint64_t> report_age_ns_;
  std::optional<std::uint64_t> progress_age_ns_;
  std::string observed_product_identity_;
  std::string observed_configuration_identity_;
  std::optional<RuntimeSourcePhase> source_phase_;
  std::optional<RuntimeSourceHealth> source_health_;
  std::optional<std::uint64_t> source_report_sequence_;
  std::optional<std::uint64_t> critical_progress_sequence_;
  std::vector<std::string> ready_runtime_role_ids_;
  std::optional<ManagedCommandSurfaceState> command_surface_state_;
  std::map<std::string, std::string> dependency_runtime_epochs_;
  std::vector<std::string> source_reason_codes_;
  std::vector<std::string> reason_codes_;
};

class RuntimeObservationSnapshot final {
 public:
  [[nodiscard]] std::uint64_t evaluated_at_ns() const noexcept;
  [[nodiscard]] const std::vector<RuntimeUnitObservation>& observations()
      const & noexcept;
  const std::vector<RuntimeUnitObservation>& observations() const && = delete;
  [[nodiscard]] const RuntimeUnitObservation* observation(
      std::string_view runtime_unit_id) const & noexcept;
  const RuntimeUnitObservation* observation(std::string_view) const && = delete;

 private:
  friend class RuntimeObservationReducer;

  RuntimeObservationSnapshot(
      std::uint64_t evaluated_at_ns,
      std::vector<RuntimeUnitObservation> observations);

  std::uint64_t evaluated_at_ns_;
  std::vector<RuntimeUnitObservation> observations_;
};

class RuntimeUnitProcessBinding final {
 public:
  RuntimeUnitProcessBinding(
      std::string runtime_unit_id,
      std::string process_boundary_id,
      RuntimeLifecycleControl lifecycle_control,
      std::string process_observation_handle_id,
      std::string status_source_id,
      std::string progress_source_id,
      std::string incarnation_source_id,
      std::string managed_lifecycle_handle_id = {});

  [[nodiscard]] const std::string& runtime_unit_id() const noexcept;
  [[nodiscard]] const std::string& process_boundary_id() const noexcept;
  [[nodiscard]] RuntimeLifecycleControl lifecycle_control() const noexcept;
  [[nodiscard]] const std::string& process_observation_handle_id()
      const noexcept;
  [[nodiscard]] const std::string& status_source_id() const noexcept;
  [[nodiscard]] const std::string& progress_source_id() const noexcept;
  [[nodiscard]] const std::string& incarnation_source_id() const noexcept;
  [[nodiscard]] const std::string& managed_lifecycle_handle_id()
      const noexcept;

 private:
  std::string runtime_unit_id_;
  std::string process_boundary_id_;
  RuntimeLifecycleControl lifecycle_control_;
  std::string process_observation_handle_id_;
  std::string status_source_id_;
  std::string progress_source_id_;
  std::string incarnation_source_id_;
  std::string managed_lifecycle_handle_id_;
};

void ValidateRuntimeUnitProcessBindings(
    const std::vector<RuntimeUnitProcessBinding>& bindings);

struct RuntimeObservationReducerMetrics final {
  std::uint64_t accepted_presence_total{};
  std::uint64_t accepted_source_report_total{};
  std::uint64_t duplicate_total{};
  std::uint64_t rejected_total{};
  std::map<std::string, std::uint64_t> rejection_reasons;
};

class RuntimeObservationReducer final {
 public:
  using Clock = std::function<std::uint64_t()>;

  RuntimeObservationReducer(
      Clock clock_ns,
      std::vector<std::string> declared_runtime_unit_ids);
  ~RuntimeObservationReducer();
  RuntimeObservationReducer(RuntimeObservationReducer&&) noexcept;
  RuntimeObservationReducer& operator=(RuntimeObservationReducer&&) noexcept;
  RuntimeObservationReducer(const RuntimeObservationReducer&) = delete;
  RuntimeObservationReducer& operator=(const RuntimeObservationReducer&) =
      delete;

  [[nodiscard]] const std::vector<std::string>& declared_runtime_unit_ids()
      const noexcept;
  [[nodiscard]] RuntimeObservationReducerMetrics metrics() const;

  [[nodiscard]] bool ObserveProcess(
      const ProcessPresenceObservation& sample);
  [[nodiscard]] bool ReplaceProcessObserver(
      const ProcessPresenceObservation& sample);
  [[nodiscard]] bool IngestSourceReport(
      const RuntimeUnitSourceReport& report);
  [[nodiscard]] RuntimeObservationSnapshot Evaluate(
      const std::vector<RuntimeUnitExpectation>& expectations);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view ToString(RuntimeLifecycleControl value);
[[nodiscard]] std::string_view ToString(ProcessPresenceState value);
[[nodiscard]] std::string_view ToString(RuntimeSourcePhase value);
[[nodiscard]] std::string_view ToString(RuntimeSourceHealth value);
[[nodiscard]] std::string_view ToString(ManagedCommandSurfaceState value);
[[nodiscard]] std::string_view ToString(RuntimeConvergenceState value);

}  // namespace manyforge::component_manager
