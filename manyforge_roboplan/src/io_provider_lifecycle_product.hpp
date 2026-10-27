#pragma once

#include <functional>
#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_core/io_provider_lifecycle_catalog.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_roboplan/geometric_contact_input_provider.hpp>
#include <manyforge_roboplan/mock_boolean_input_provider.hpp>
#include <manyforge_roboplan/mock_boolean_output_provider.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/ros_boolean_input_provider.hpp>
#include <manyforge_roboplan/ros_boolean_output_provider.hpp>
#include <manyforge_roboplan/xarm_digital_input_provider.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <variant>

namespace manyforge::roboplan_backend::host_detail {

struct IoProviderInputSourceInstallation final {
  std::vector<deployment::RetainedIoSignalConfiguration> rows;
  rclcpp::Node::SharedPtr node;
  std::shared_ptr<core::IIoSignalIngress> ingress;
  core::IoSourceIncarnation incarnation;
  std::optional<GeometricContactInputBinding> geometric_contact;
  std::shared_ptr<core::IScheduler> scheduler{};
};

struct IoProviderOutputChannelInstallation final {
  core::ResourceId resource_id;
  // Only this record's sealed aliases; the actual shared channel may also
  // serve other component/domain occurrences with independent gates.
  std::vector<deployment::RetainedIoSignalConfiguration> rows;
  std::variant<std::shared_ptr<MockBooleanOutputProvider>,
               std::shared_ptr<RosBooleanOutputProvider>>
      provider;
};

struct IoProviderInputAcquisitionBaseline final {
  core::IoSourceIncarnation incarnation;
  std::uint64_t source_sequence{};
  std::vector<core::IoSignalId> signals;
};

/// The actual generic I/O driver occurrence. The existing CCLC transaction
/// remains the lifecycle authority; this record owns its real driver facts,
/// delivery mutex, leaves and retained brand-port observation sequence.
/// Arm-hosted GPIO belongs to the existing arm driver and never enters here.
class IoProviderDriverRecord final
    : public std::enable_shared_from_this<IoProviderDriverRecord> {
public:
  using InputLifecycleHook = std::function<core::Result<void>(
      const core::LifecycleTransactionId &, bool planned_disabled)>;
  using InputAcquisitionHook = std::function<core::Result<void>(
      std::span<const IoProviderInputAcquisitionBaseline>)>;
  static core::Result<std::shared_ptr<IoProviderDriverRecord>>
  Create(core::ResolvedLifecycleControlDomainScopeDTO domain,
         core::ResolvedLifecycleProviderOccurrenceDTO occurrence,
         core::ProviderManifestDTO manifest,
         std::shared_ptr<core::IClock> clock,
         core::Duration observation_freshness_ns);

  ~IoProviderDriverRecord();
  IoProviderDriverRecord(const IoProviderDriverRecord &) = delete;
  IoProviderDriverRecord &operator=(const IoProviderDriverRecord &) = delete;

  const core::ResolvedLifecycleControlDomainScopeDTO &domain() const noexcept;
  const core::ResolvedLifecycleProviderOccurrenceDTO &
  occurrence() const noexcept;
  const core::BrandLifecyclePlanCatalogDTO &catalog() const noexcept;
  const core::ProviderManifestDTO &manifest() const noexcept;
  core::IClock &clock() const noexcept;
  core::BrandLifecyclePortRegistrationDTO brand_registration() const;
  core::RuntimeUnitEpochBindingDTO runtime_unit_epoch() const;
  bool resources_installed() const;

  // Host timer/registration/release code takes this same barrier before its
  // final token check. Runtime timer-map locks must never wait for this lock.
  std::unique_lock<std::mutex> AcquireDelivery() const;
  std::uint64_t driver_observation_generation_locked() const noexcept;

  // Allocate the real resources exactly once before exposing this record.
  // Output aliases are created paused using the generation read above; input
  // subscriptions are created paused here before any callback may publish.
  core::Result<void> InstallResourcesLocked(
      std::vector<IoProviderInputSourceInstallation> inputs,
      std::vector<IoProviderOutputChannelInstallation> outputs,
      std::function<void()> cancel_pending_answers,
      InputLifecycleHook input_lifecycle = {},
      InputAcquisitionHook input_acquisition = {});
  core::Result<MockBooleanInputSetReceipt>
  SetMockValueLocked(const core::IoSignalId &signal, bool value);

  // Same exact qualified occurrence/membership only. The host fences/drains
  // old runtime timers first. New host incarnations bind actual new Apply;
  // exact replay cannot reset the model or resend an acquisition.
  core::Result<void>
  RebindInputsLocked(std::vector<IoProviderInputSourceInstallation> inputs,
                     std::function<void()> cancel_pending_answers,
                     InputLifecycleHook input_lifecycle = {},
                     InputAcquisitionHook input_acquisition = {});

  core::Result<RetainedExternallyManagedDomainSnapshot>
  ReadLifecycleSnapshot(const core::ControlDomainId &domain,
                        core::TimePoint now);
  core::Result<core::LifecycleDomainHealthDTO>
  ReadLifecycleHealth(const core::ControlDomainId &domain,
                      core::TimePoint now) const;
  RetainedExternallyManagedPrimitiveOutcome
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token cancellation);

  // sim-plc / pose source: the exact supervised process serving this
  // component. Its typed exit fault (absent while it runs or after an ordered
  // stop; the source names the code, e.g. `simulated_plc_exited`) is a driver
  // fault: prepare/enable/recover are refused and the standard cause is
  // `fault`. A later Apply installs the restarted process's source,
  // replacing this one; an empty source removes it.
  void SetSupervisedProcessFaultSource(
      std::function<std::optional<core::ErrorDTO>()> source);

  // CP9 model-free stream observers: pose source
  // (`kPoseSourceLifecycleManifestId`) and AI model
  // (`kAiModelLifecycleManifestId`). No I/O members. Its resources are the
  // declared stream recorders (ids equal to the domain's signal names: one
  // per pose binding, or the AI model's `endpoint`); installed means
  // resources and transport ready. The recorders live until Shutdown
  // (destruction undeclares).
  [[nodiscard]] bool stream_observer() const noexcept;
  core::Result<void> InstallObserverStreamsLocked(
      std::vector<std::unique_ptr<core::StreamRecorder>> streams);

  // Only the actual retiring driver owner calls this. An old activation that
  // transferred this retained record must release its pointer without it.
  void Shutdown();

private:
  struct State;
  explicit IoProviderDriverRecord(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend::host_detail
