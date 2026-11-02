#pragma once

#include <manyforge_core/ports/iio_signal_service.hpp>
#include <manyforge_core/types/io_rule.hpp>
#include <memory>
#include <span>

namespace manyforge::core {

/// One module in the existing host. No threads, provider calls or effects.
/// Its lifetime is the supplied owner epoch/catalog generation; recreating the
/// module requires a new generation before exposing any registration token.
class IoSignalSubsystem final : public IIoSignalService {
 public:
  static Result<std::unique_ptr<IoSignalSubsystem>> Create(IoSignalCatalogDTO catalog,
                                                           IoSignalLimitsDTO limits, IClock& clock);
  ~IoSignalSubsystem() override;

  IoSignalSubsystem(const IoSignalSubsystem&) = delete;
  IoSignalSubsystem& operator=(const IoSignalSubsystem&) = delete;

  Result<void> InstallProgramBlackboardSource(
      const IoProgramBlackboardSourceBindingDTO& binding,
      const std::optional<IoSourceIncarnation>& expected_current_incarnation) override;
  Result<IoProgramBlackboardAcknowledgementDTO> PublishProgramBlackboardBaseline(
      const IoProgramBlackboardSourceBindingDTO& binding,
      const std::optional<IoSourceIncarnation>& expected_current_incarnation,
      const IoProgramBlackboardPublicationDTO& publication) override;
  Result<IoProgramBlackboardAcknowledgementDTO> PublishProgramBlackboard(
      const IoProgramBlackboardPublicationDTO& publication) override;
  Result<void> RetireProgramBlackboardSource(
      const IoProgramBlackboardSourceBindingDTO& binding) override;

  Result<bool> WasProgramBlackboardScopeRetired(
      const IoProgramBlackboardScopeDTO& scope) const override;

  /// Existing provider authority supplies a unique, never-reused incarnation.
  /// The exact predecessor fences delayed installation calls; null means the
  /// source has never been installed. An exact already-installed retry is inert.
  Result<void> InstallSource(
      const InterruptSourceId& source, const IoSourceIncarnation& incarnation,
      const std::optional<IoSourceIncarnation>& expected_current_incarnation);
  /// Trusted in-process host boundary: capture receiver time with this IClock
  /// before enqueue, then preserve it through drain. Never take device/ROS time
  /// or an external client's asserted receipt time. Future/regressed stamps
  /// refuse before mutation; an already expired sample cannot satisfy a watch.
  Result<IoSourceCursorDTO> Publish(const IoSourceAcquisitionDTO& acquisition);
  /// Non-mutating admission for the host queue; Publish rechecks at drain.
  Result<void> ValidateAcquisition(const IoSourceAcquisitionDTO& acquisition) const;
  Result<IoSignalIngressConfigurationDTO> IngressConfiguration() const;
  Result<void> ReportIngressLoss(const IoIngressLossBarrierDTO& barrier);
  /// One locked observation snapshot; physical acquisitions across sources are
  /// still independent. All IDs validate before producing any observation.
  Result<std::vector<IoSignalObservationDTO>> ReadSignals(
      std::span<const IoSignalId> signals) const;
  /// Availability loss is ordered against the exact last accepted publication.
  Result<void> MarkSourceUnavailable(const InterruptSourceId& source,
                                     const IoSourceIncarnation& incarnation,
                                     std::uint64_t expected_source_sequence, const ErrorDTO& error);
  /// Actual ingress loss, distinct from overwriting already evaluated history.
  Result<void> ReportIngressOverflow(const InterruptSourceId& source,
                                     const IoSourceIncarnation& incarnation, const ErrorDTO& error);
  Result<IoSignalRecordPageDTO> ReadRecords(const IoSourceCursorDTO& after,
                                            std::size_t maximum_records) const;

  /// Existing operation/activation owner controls applicability and retires
  /// exact tokens. No idle/rearm/outcome/Stop policy is decided in this module.
  Result<IoRuleServiceConfigurationDTO> RuleServiceConfiguration() const override;
  Result<IoRuleRegistrationDTO> ArmRule(const IoRuleArmRequestDTO& request) override;
  Result<IoRuleConditionSnapshotDTO> ReadRuleConditionSnapshot(
      const IoRuleRegistrationDTO& registration) const override;
  Result<IoRuleEvidenceDTO> ReadRuleEvidence(
      const IoRuleRegistrationDTO& registration) const override;
  /// Atomically capture final acquired evidence and retire; no late evaluation.
  Result<IoRuleEvidenceDTO> RetireRule(const IoRuleRegistrationDTO& registration) override;
  /// Existing host scheduling calls this for age-only failures even if no new
  /// acquisition arrives. It adds no thread/timer; Publish also checks expiry
  /// before overwriting prior knowledge. Evidence reads do not evaluate/consume.
  Result<void> EvaluateRules() override;
  Result<std::optional<TimePoint>> NextRuleExpiry() const override;

  Result<void> ClearOutsideChange(const IoSignalId& output) override;
  Result<void> ClearComponentOutsideChanges(const ControllableComponentId& component) override;
  Result<IoSignalObservationDTO> ReadSignal(const IoSignalId& signal) const override;
  /// Every mutation above raises the event-stream change signal when a
  /// signal's value, source or freshness changed (never for age alone). A
  /// fresh sample turns stale with no mutation: the host calls
  /// `RefreshSamples` at `NextSampleExpiry` (the earliest such moment; empty
  /// when no age-checked signal is fresh).
  [[nodiscard]] std::optional<TimePoint> NextSampleExpiry() const;
  void RefreshSamples();
  Result<IoWatchRegistrationDTO> RegisterLevelWatch(const IoLevelWatchRequestDTO& request) override;
  Result<IoWatchResultDTO> ReadWatch(const IoWatchRegistrationDTO& registration) const override;
  Result<void> ReleaseWatch(const IoWatchRegistrationDTO& registration) override;
  Result<void> ReleaseNodeWatches(const IoWatchScopeDTO& scope) override;
  Result<void> ReleaseRunWatches(const RequestOwnerEpoch& runner_owner_epoch,
                                 const ProgramRunId& run_id) override;

 private:
  struct State;
  explicit IoSignalSubsystem(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

}  // namespace manyforge::core
