#pragma once

#include <functional>
#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/ports/icomponent_adapter_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/lifecycle_provider_occurrence.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_roboplan/geometric_contact_input_provider.hpp>
#include <manyforge_roboplan/io_output_writer.hpp>
#include <manyforge_roboplan/mock_boolean_input_provider.hpp>
#include <manyforge_roboplan/mock_boolean_output_provider.hpp>
#include <manyforge_roboplan/ros_boolean_input_provider.hpp>
#include <manyforge_roboplan/ros_boolean_output_provider.hpp>
#include <memory>
#include <mutex>
#include <span>
#include <variant>

namespace manyforge::roboplan_backend::host_detail {

/// The provider manifests of the host's generic I/O driver: I/O signals, and
/// the model-free stream observers (pose source, AI model).
inline constexpr std::string_view kIoProviderManifestId = "manyforge.provider.io_signals";
inline constexpr std::string_view kPoseSourceProviderManifestId =
    "manyforge.provider.pose_source";
inline constexpr std::string_view kAiModelProviderManifestId = "manyforge.provider.ai_model";
/// IO-1: a generic I/O device's optional silence stream (its ROS inputs).
inline constexpr std::string_view kIoDeviceInputsStreamId = "inputs";

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

/// The actual generic I/O driver occurrence and the component adapter of its
/// I/O provider component: it owns its real driver facts,
/// delivery mutex and leaves, reports the component's state and runs its
/// lifecycle actions. Arm-hosted GPIO belongs to the arm's adapter and never
/// enters here.
class IoProviderDriverRecord final
    : public core::IComponentAdapter,
      public IIoOutputWriter,
      public std::enable_shared_from_this<IoProviderDriverRecord> {
public:
  using InputLifecycleHook = std::function<core::Result<void>(bool planned_disabled)>;
  using InputAcquisitionHook = std::function<core::Result<void>(
      std::span<const IoProviderInputAcquisitionBaseline>)>;
  static core::Result<std::shared_ptr<IoProviderDriverRecord>>
  Create(core::ResolvedLifecycleControlDomainScopeDTO domain,
         core::ResolvedLifecycleProviderOccurrenceDTO occurrence,
         core::ProviderManifestDTO manifest,
         std::shared_ptr<core::IClock> clock);

  ~IoProviderDriverRecord() override;
  IoProviderDriverRecord(const IoProviderDriverRecord &) = delete;
  IoProviderDriverRecord &operator=(const IoProviderDriverRecord &) = delete;

  const core::ResolvedLifecycleControlDomainScopeDTO &domain() const noexcept;
  const core::ResolvedLifecycleProviderOccurrenceDTO &
  occurrence() const noexcept;
  const core::ProviderManifestDTO &manifest() const noexcept;
  core::IClock &clock() const noexcept;
  core::RuntimeUnitEpochBindingDTO runtime_unit_epoch() const;
  bool resources_installed() const;
  // Atomic published identity; this does not acquire the delivery barrier.
  [[nodiscard]] std::shared_ptr<ros2_control_execution::Ros2TransportResources>
  RosOwner() const noexcept;

  // Host timer/registration/release code takes this same barrier before its
  // final token check. Runtime timer-map locks must never wait for this lock.
  std::unique_lock<std::mutex> AcquireDelivery() const;
  std::uint64_t driver_observation_generation_locked() const noexcept;

  struct RosOwnerRecoverySnapshot;
  struct RosOwnerRecoveryStage;
  [[nodiscard]] bool UsesRosOwnerLocked(
      const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &owner) const;
  [[nodiscard]] core::Result<std::shared_ptr<RosOwnerRecoverySnapshot>>
  CloseAndCaptureRosOwnerRecovery(
      const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &owner,
      bool bridge_observer = false);
  [[nodiscard]] static core::Result<std::shared_ptr<RosOwnerRecoveryStage>>
  StageRosOwnerRecovery(
      std::shared_ptr<RosOwnerRecoverySnapshot> snapshot,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner);
  // Caller holds this record's delivery barrier; all snapshots in the batch
  // are checked before any Commit call.
  [[nodiscard]] bool MatchesRosOwnerRecoveryLocked(
      const RosOwnerRecoverySnapshot &snapshot) const noexcept;
  void CommitRosOwnerRecoveryLocked(RosOwnerRecoveryStage &stage) noexcept;
  static void ActivateRosOwnerRecovery(RosOwnerRecoveryStage &stage) noexcept;

  // Allocate the real resources exactly once before exposing this record.
  // Output aliases are created paused using the generation read above; input
  // subscriptions are created paused here before any callback may publish.
  core::Result<void> InstallResourcesLocked(std::unique_lock<std::mutex> &delivery,
      std::vector<IoProviderInputSourceInstallation> inputs,
      std::vector<IoProviderOutputChannelInstallation> outputs,
      std::function<void()> cancel_pending_answers,
      InputLifecycleHook input_lifecycle = {},
      InputAcquisitionHook input_acquisition = {},
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner = {});
  core::Result<MockBooleanInputSetReceipt>
  SetMockValueLocked(const core::IoSignalId &signal, bool value);

  // Same exact qualified occurrence/membership only. The host fences/drains
  // old runtime timers first. New host incarnations bind actual new Apply;
  // exact replay cannot reset the model or resend an acquisition.
  core::Result<void>
  RebindInputsLocked(std::unique_lock<std::mutex> &delivery,
                     std::vector<IoProviderInputSourceInstallation> inputs,
                     std::function<void()> cancel_pending_answers,
                     InputLifecycleHook input_lifecycle = {},
                     InputAcquisitionHook input_acquisition = {},
                     std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner = {});

  // State: `fault` for a driver fault, a lost transport while
  // enabled or an exited supervised process; `offline` while the configured
  // transport isn't reachable; streams from the host stream registry.
  // IO-1 senders gone: once a ROS input topic had a publisher since install,
  // every ROS input topic without one is `fault` (`fault_communication`)
  // while enabled, held until Recover or Disable, and `offline` (reason
  // `io_device_senders_gone`) otherwise; prepare, enable and recover are
  // refused with that code until a sender is back. Matched-publisher counts
  // (ROS matched events) are read on the host's component tick. A stale
  // `inputs` stream is `fault_stream_stale`.
  // Actions: prepare (installed, reachable), enable (from prepared: resumes
  // the leaves), disable (pauses them), recover (clears a driver fault and
  // pauses). Capability `io_manual_write` when the component has outputs,
  // plus `io_output_readback` when some are Mock outputs.
  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override;
  // Test observation: the matched compatible publisher count summed over the
  // installed ROS input leaves, the count IO-1 reads.
  [[nodiscard]] std::size_t RosInputMatchedPublishersForTest() const;
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token stop) override;
  [[nodiscard]] std::vector<core::Capability> capabilities() const override;

  // A manual write of one of this component's outputs,
  // through the output provider of its channel (Mock or ROS bool). The
  // channel's own gate refuses it while the component isn't enabled; a
  // controller-owned output is refused before sending.
  [[nodiscard]] SetOutputResult SetOutput(std::string_view signal_name,
                                          std::variant<bool, double> value,
                                          std::stop_token stop) override;

  // sim-plc / pose source: the exact supervised process serving this
  // component. Its typed exit fault (absent while it runs or after an ordered
  // stop; the source names the code, e.g. `simulated_plc_exited`) is a driver
  // fault: prepare/enable/recover are refused and the component is in
  // `fault_process_exited`. A later Apply installs the restarted process's source,
  // replacing this one; an empty source removes it.
  void SetSupervisedProcessFaultSource(
      std::function<std::optional<core::ErrorDTO>()> source);

  // CP9 model-free stream observers: pose source
  // (`kPoseSourceProviderManifestId`) and AI model
  // (`kAiModelProviderManifestId`). No I/O members. Its resources are the
  // declared stream recorders (ids equal to the domain's signal names: one
  // per pose binding, or the AI model's `endpoint`); installed means
  // resources and transport ready. The recorders live until Shutdown
  // (destruction undeclares).
  [[nodiscard]] bool stream_observer() const noexcept;
  [[nodiscard]] bool InstalledStreamObserver() const noexcept;
  core::Result<void> InstallObserverStreamsLocked(
      std::vector<std::unique_ptr<core::StreamRecorder>> streams);

  // IO-1 silence budget: with `budget_ns` (the Deployment's
  // `policies.component_streams` row for `(device, "inputs")`) a device with
  // ROS inputs declares its `inputs` stream (a stale one faults the device);
  // every message on those inputs records an arrival, also while paused.
  // Without a row the stream is removed. A kept stream follows the
  // registry's overrides for its budget.
  core::Result<void> SetInputsStreamLocked(std::optional<std::int64_t> budget_ns);

  // Only the actual retiring driver owner calls this. An old activation that
  // transferred this retained record must release its pointer without it.
  void Shutdown();

private:
  struct Driver;
  explicit IoProviderDriverRecord(std::unique_ptr<Driver> state);
  std::unique_ptr<Driver> state_;
};

// Retained pins keep this in-process tie-breaker stable for the entire lock
// acquisition, including separate records with the same provider instance ID.
inline bool DeliveryLockOrder(const std::shared_ptr<IoProviderDriverRecord> &left,
                              const std::shared_ptr<IoProviderDriverRecord> &right) noexcept {
  const auto &left_id = left->occurrence().provider_instance_id.value();
  const auto &right_id = right->occurrence().provider_instance_id.value();
  if (left_id != right_id) return left_id < right_id;
  return std::less<const IoProviderDriverRecord *>{}(left.get(), right.get());
}

} // namespace manyforge::roboplan_backend::host_detail
