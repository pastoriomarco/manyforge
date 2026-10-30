// End-to-end CCLC lifecycle transactions on the maintained Real xArm and
// shared ros2_control plans: the coordinator, its SQLite journal, the plan
// evaluator and (V-B2) the real brand port running each transition through
// the shared runner, with scripted device reports and substep outcomes
// (REC FIX5).
#include "../../manyforge_core/test/cclc_test_admission_port.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/cclc_domain_reservation_table.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <manyforge_xarm_control/lifecycle_profile.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using core::ComponentLifecycleIntentKindDTO;
using core::ComponentLifecycleStateDTO;
using State = core::ComponentLifecycleStateDTO;
using Intent = core::ComponentLifecycleIntentKindDTO;

constexpr std::string_view kRevision =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

[[nodiscard]] std::string Sha(char value) {
  return std::string("sha256:") + std::string(64U, value);
}

class TemporaryRoot final {
public:
  TemporaryRoot() {
    static std::atomic<int> next{1};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-real-xarm-cclc-" + std::to_string(::getpid()) + "-" +
             std::to_string(next.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }
  ~TemporaryRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] core::BrandLifecyclePlanCatalogDTO
Catalog(std::string_view id, std::vector<core::LifecycleIntentPlanDTO> plans) {
  auto made = core::MakeBrandLifecyclePlanCatalog(
      core::ProviderManifestId(std::string(id)),
      core::ProviderManifestRevision(std::string(kRevision)), std::move(plans));
  if (!made.ok()) throw std::logic_error(made.error().message);
  return made.value();
}

[[nodiscard]] core::LifecycleFactDTO Fact(std::string_view id, bool value) {
  return {core::LifecycleFactId(std::string(id)),
          {core::LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
           std::nullopt}};
}

/// One fresh provider report: a lifecycle state and its facts.
struct Report final {
  State state;
  std::vector<core::LifecycleFactDTO> facts;
};

[[nodiscard]] Report Sorted(Report report) {
  std::sort(report.facts.begin(), report.facts.end(),
            [](const auto &left, const auto &right) {
              return left.lifecycle_fact_id < right.lifecycle_fact_id;
            });
  return report;
}

/// The Real xArm arm, never armed (vendor error cleared): Declared.
[[nodiscard]] Report UnarmedArm() {
  return Sorted({State::kDeclared,
                 {Fact(core::kDeviceAcceptsCommandsFactId, false),
                  Fact(core::kDeviceErrorPresentFactId, false),
                  Fact(core::kDeviceWarningPresentFactId, false),
                  Fact("external.broadcaster_active", true),
                  Fact("external.controller_active", false),
                  Fact("external.controller_configuration_ready", false),
                  Fact("external.controller_loaded", false),
                  Fact("external.controller_spawned", true),
                  Fact("external.fault_present", false),
                  Fact("external.hardware_active", false),
                  Fact("external.measured_joints_valid", true),
                  Fact("external.transport_ready", true)}});
}

/// The unarmed arm with one fact changed (negatives of the unarmed proof).
[[nodiscard]] Report ArmWith(std::vector<std::pair<std::string_view, bool>> changes) {
  auto report = UnarmedArm();
  for (const auto &[id, value] : changes)
    for (auto &fact : report.facts)
      if (fact.lifecycle_fact_id.value() == id) fact.value.boolean_value = value;
  return report;
}

/// `report` with these facts unknown (the vendor status stale or missing).
[[nodiscard]] Report WithUnknown(Report report, std::vector<std::string_view> ids) {
  for (const auto id : ids)
    for (auto &fact : report.facts)
      if (fact.lifecycle_fact_id.value() == id)
        fact.value = {core::LifecycleFactValueKindDTO::kUnknown, std::nullopt,
                      std::nullopt, std::nullopt};
  return report;
}

/// A ros2_control gripper (shared plans' facts) in `state`.
[[nodiscard]] Report Gripper(State state) {
  const bool active = state == State::kActive;
  const bool loaded = state == State::kPrepared || active;
  return Sorted({state,
                 {Fact("external.controller_configuration_ready", active),
                  Fact("external.controller_loaded", loaded),
                  Fact("external.fault_present", false),
                  Fact("external.transport_ready", true)}});
}

using Outcome = RetainedRos2ControlSwitchOutcome;
using Terminal = core::ComponentLifecycleTerminalResultDTO;

/// The clock of the coordinator and of every port: observations and the
/// transaction's time on one clock, as in the host.
[[nodiscard]] core::FakeClock &TestClock() {
  static core::FakeClock clock;
  return clock;
}

/// A provider's device behind the real brand port: one current report; a
/// dispatched substep moves it to that substep's scripted report and returns
/// its scripted handle outcome (a success unless overridden).
class ScriptedDevice final {
public:
  ScriptedDevice(Report current, std::map<std::string, Report> after)
      : current_(std::move(current)), after_(std::move(after)) {}

  void SetAfter(const std::string &primitive, Report report) {
    std::lock_guard lock(mutex_);
    after_.insert_or_assign(primitive, std::move(report));
  }
  void SetCurrent(Report report) {
    std::lock_guard lock(mutex_);
    current_ = std::move(report);
  }
  void SetResult(const std::string &primitive,
                 std::optional<RetainedExternallyManagedPrimitiveOutcome> result) {
    std::lock_guard lock(mutex_);
    if (result)
      results_.insert_or_assign(primitive, std::move(*result));
    else
      results_.erase(primitive);
  }

  /// Every read is a new source generation.
  [[nodiscard]] core::Result<RetainedExternallyManagedDomainSnapshot> Read() {
    std::lock_guard lock(mutex_);
    return core::Result<RetainedExternallyManagedDomainSnapshot>::Ok(
        {++generation_, current_.state, false, current_.facts, std::nullopt, nullptr});
  }

  [[nodiscard]] RetainedExternallyManagedPrimitiveOutcome
  Dispatch(const core::LifecyclePrimitiveId &primitive) {
    std::lock_guard lock(mutex_);
    dispatched_.push_back(primitive.value());
    const auto found = after_.find(primitive.value());
    if (found != after_.end()) current_ = found->second;
    if (const auto scripted = results_.find(primitive.value()); scripted != results_.end())
      return scripted->second;
    return {Outcome::kSucceeded, std::nullopt};
  }

  /// The substeps dispatched, in order.
  [[nodiscard]] std::vector<std::string> dispatched() const {
    std::lock_guard lock(mutex_);
    return dispatched_;
  }

private:
  mutable std::mutex mutex_;
  Report current_;
  std::map<std::string, Report> after_;
  std::map<std::string, RetainedExternallyManagedPrimitiveOutcome> results_;
  std::vector<std::string> dispatched_;
  std::uint64_t generation_ = 0U;
};

/// The real port of one domain, keeping its last transition result.
class RecordingPort final : public core::IBrandLifecyclePort {
public:
  explicit RecordingPort(std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort> port)
      : port_(std::move(port)) {}

  core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override {
    return port_->Observe(query);
  }
  const core::ILifecycleAdmission &Admission() const override { return port_->Admission(); }
  core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           const core::LifecycleSubstepFeedbackSink &feedback,
           std::stop_token cancellation) override {
    auto result = port_->Dispatch(intent, feedback, cancellation);
    std::lock_guard lock(mutex_);
    last_ = result;
    return result;
  }

  [[nodiscard]] std::optional<core::BrandLifecyclePrimitiveResultDTO> last() const {
    std::lock_guard lock(mutex_);
    return last_;
  }

private:
  std::shared_ptr<RetainedExternallyManagedBrandLifecyclePort> port_;
  mutable std::mutex mutex_;
  std::optional<core::BrandLifecyclePrimitiveResultDTO> last_;
};

/// The two domains of one Real Lite 6 component, arm before gripper.
struct DomainSetup final {
  std::string domain;
  std::string instance;
  core::BrandLifecyclePlanCatalogDTO catalog;
  std::shared_ptr<ScriptedDevice> device;
  std::shared_ptr<RecordingPort> port;
};

[[nodiscard]] core::ExpectedControlDomainRevisionDTO
Expected(const DomainSetup &setup) {
  return {core::ControlDomainId(setup.domain),
          core::ControlDomainRevision("domain-revision"),
          core::ControllerTopologyId("topology." + setup.domain),
          core::TopologyRevision("topology-revision"),
          {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
           std::nullopt, std::nullopt},
          {{core::ControlBindingId("binding." + setup.domain),
            core::ControlBindingRevision("binding-revision"),
            core::ControlRouteId("route." + setup.domain)}}};
}

[[nodiscard]] core::ResolvedLifecycleProviderOccurrenceDTO
Occurrence(const std::string &domain, const std::string &instance,
           const core::BrandLifecyclePlanCatalogDTO &catalog) {
  return {core::ControlDomainId(domain),
          core::ControlDomainRevision("domain-revision"),
          core::ProviderInstanceId(instance),
          catalog.provider_manifest_id,
          catalog.provider_manifest_revision,
          core::RuntimeUnitId("robot_cell.ros2_control_execution"),
          core::RuntimeEpoch("runtime-epoch.1"),
          catalog.catalog_digest};
}

[[nodiscard]] core::ResolvedLifecycleProviderOccurrenceDTO
Occurrence(const DomainSetup &setup) {
  return Occurrence(setup.domain, setup.instance, setup.catalog);
}

/// One domain: its scripted device behind the real brand port, the device's
/// substeps reached through the port's dispatcher.
[[nodiscard]] DomainSetup MakeDomain(std::string domain, std::string instance,
                                core::BrandLifecyclePlanCatalogDTO catalog,
                                std::shared_ptr<ScriptedDevice> device) {
  auto port = RetainedExternallyManagedBrandLifecyclePort::Create(
      Occurrence(domain, instance, catalog), catalog,
      [device](const core::ControlDomainId &, core::TimePoint) { return device->Read(); },
      TestClock(), 1'000'000'000LL, 1'000'000LL,
      [device](const core::ControlDomainId &, const core::LifecyclePrimitiveId &primitive,
               std::stop_token, std::optional<core::Duration>,
               const core::LifecycleSubstepFeedbackSink &) {
        return device->Dispatch(primitive);
      });
  if (!port.ok()) throw std::logic_error(port.error().message);
  return {std::move(domain), std::move(instance), std::move(catalog), std::move(device),
          std::make_shared<RecordingPort>(std::move(port.value()))};
}

/// The substeps of a domain's last transition: (id, outcome).
[[nodiscard]] std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>
Substeps(const DomainSetup &setup) {
  std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>> result;
  if (const auto last = setup.port->last())
    for (const auto &substep : last->substeps)
      result.emplace_back(substep.substep_id.value(), substep.outcome);
  return result;
}

[[nodiscard]] core::ComponentLifecycleCurrentSnapshotDTO
Snapshot(const std::vector<DomainSetup> &domains, State state,
         std::uint64_t generation) {
  const core::ProjectionRevisionDTO revision{
      core::RetainedBundleId("bundle.1"), core::GraphRevision("graph.1"),
      {core::SceneId(Sha('a')), 1U, core::ContentDigest(Sha('b'))}};
  core::ComponentLifecycleCurrentSnapshotDTO snapshot{
      core::ContentDigest(Sha('0')),
      core::ControllableComponentId("lite6_robot"),
      core::ComponentLifecycleGeneration(generation),
      state,
      {{core::ActiveBundlePublicationTransactionRefDTO{
            core::BootstrapPublicationTransactionDTO{
                core::BootstrapId("bootstrap.1"),
                core::BootstrapGeneration(1U)}},
        revision, revision.retained_bundle_id, revision.graph_revision,
        std::nullopt, core::ContentDigest("private.1")},
       core::ActiveBundlePublicationGeneration(2U),
       core::ContentDigest("public-pointer.1")},
      {},
      {}};
  for (const auto &setup : domains) {
    const auto expected = Expected(setup);
    snapshot.domains.push_back(
        {expected.control_domain_id,
         expected.control_domain_revision,
         expected.controller().controller_topology_id,
         expected.controller().topology_revision,
         std::nullopt,
         expected.controller().expected_control_bindings,
         {core::ResourceId("resource." + setup.domain)}});
    snapshot.provider_occurrences.push_back(Occurrence(setup));
  }
  const auto digest = core::ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
  if (!digest.ok()) throw std::logic_error(digest.error().message);
  snapshot.snapshot_digest = digest.value();
  return snapshot;
}

class SequenceSnapshotSource final
    : public core::ICCLCCurrentComponentSnapshotSource {
public:
  void Set(std::vector<core::ComponentLifecycleCurrentSnapshotDTO> sequence) {
    std::lock_guard lock(mutex_);
    sequence_ = std::move(sequence);
    reads_ = 0U;
  }
  core::Result<core::ComponentLifecycleCurrentSnapshotDTO>
  ReadCurrentComponentSnapshot(const core::ControllableComponentId &) const override {
    std::lock_guard lock(mutex_);
    const auto index = std::min(reads_++, sequence_.size() - 1U);
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Ok(
        sequence_[index]);
  }

private:
  mutable std::mutex mutex_;
  std::vector<core::ComponentLifecycleCurrentSnapshotDTO> sequence_;
  mutable std::size_t reads_ = 0U;
};

class SetupCatalogSource final : public core::ICCLCLifecyclePlanCatalogSource {
public:
  explicit SetupCatalogSource(const std::vector<DomainSetup> &domains) {
    for (const auto &setup : domains) catalogs_.push_back(setup.catalog);
  }
  core::Result<core::BrandLifecyclePlanCatalogDTO> LoadExactLifecyclePlanCatalog(
      const core::ProviderManifestId &id, const core::ProviderManifestRevision &revision,
      const core::ContentDigest &digest) const override {
    for (const auto &catalog : catalogs_)
      if (catalog.provider_manifest_id == id &&
          catalog.provider_manifest_revision == revision &&
          catalog.catalog_digest == digest)
        return core::Result<core::BrandLifecyclePlanCatalogDTO>::Ok(catalog);
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
        {"lifecycle_catalog_mismatch", "catalog mismatch", {}});
  }

private:
  std::vector<core::BrandLifecyclePlanCatalogDTO> catalogs_;
};

class SetupResolver final : public core::ICCLCBrandLifecyclePortResolver {
public:
  explicit SetupResolver(const std::vector<DomainSetup> &domains) : domains_(domains) {}
  core::Result<std::shared_ptr<core::IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const core::ResolvedLifecycleProviderOccurrenceDTO &provider) const override {
    for (const auto &setup : domains_)
      if (Occurrence(setup) == provider)
        return core::Result<std::shared_ptr<core::IBrandLifecyclePort>>::Ok(setup.port);
    return core::Result<std::shared_ptr<core::IBrandLifecyclePort>>::Err(
        {"lifecycle_port_mismatch", "provider mismatch", {}});
  }

private:
  const std::vector<DomainSetup> &domains_;
};

/// One coordinator over a real journal; transactions run one at a time.
class RealCellLifecycle final {
public:
  explicit RealCellLifecycle(const std::vector<DomainSetup> &domains)
      : journal_(root_.path() / std::string(core::kCCLCJournalFilename),
                 core::CellId("cell.1"), 8U),
        catalogs_(domains), resolver_(domains), domains_(domains),
        coordinator_({core::RequestOwnerEpoch("cclc-owner.1"), 8U, 2U, 1'000},
                     journal_, reservations_, snapshots_, catalogs_, resolver_,
                     kernel_, TestClock(), scheduler_) {
    TestClock().set(100);
    if (!coordinator_.Start().ok()) throw std::logic_error("coordinator start");
  }

  /// Submits `intent` from the component at (`state`, `generation`) and runs
  /// it to its terminal record. Disable re-reads its accepted snapshot.
  [[nodiscard]] std::optional<core::ComponentLifecycleStatusDTO>
  Run(Intent intent, State state, std::uint64_t generation, std::string request) {
    const auto current = Snapshot(domains_, state, generation);
    auto accepted = Snapshot(domains_, state, generation + 1U);
    snapshots_.Set(intent == Intent::kDisable
                       ? std::vector{current, current, accepted}
                       : std::vector{current});
    std::vector<core::ExpectedControlDomainRevisionDTO> expected;
    for (const auto &setup : domains_) expected.push_back(Expected(setup));
    const core::ComponentLifecycleIntentDTO request_intent{
        {core::RequestId(std::move(request)), coordinator_.request_owner_epoch(),
         "test", std::nullopt},
        core::ControllableComponentId("lite6_robot"),
        intent,
        core::GraphRevision("graph.1"),
        core::ComponentLifecycleGeneration(generation),
        std::move(expected),
        std::nullopt,
        {core::RequestTransportDTO::kInternal,
         core::RequestAssuranceDTO::kUnverified, std::nullopt}};
    const auto submitted = coordinator_.SubmitLifecycle(request_intent);
    if (!submitted.ok() || !submitted.value().acceptance) {
      ADD_FAILURE() << (submitted.ok() ? std::string("not accepted")
                                       : submitted.error().code + ": " +
                                             submitted.error().message);
      return std::nullopt;
    }
    static_cast<void>(coordinator_.DrainFor(std::chrono::seconds(2)));
    const auto status = coordinator_.LoadLifecycleStatus(
        submitted.value().acceptance->lifecycle_transaction_id);
    if (!status.ok() || !status.value()) return std::nullopt;
    return *status.value();
  }

  [[nodiscard]] std::optional<State> PersistedState() {
    const auto fact = journal_.LoadCurrentComponentLifecycleFact(
        core::ControllableComponentId("lite6_robot"));
    return fact.ok() ? fact.value().current_state : std::nullopt;
  }
  [[nodiscard]] bool Accepting() const { return coordinator_.accepting_new_requests(); }

private:
  TemporaryRoot root_;
  core::SqliteCCLCJournal journal_;
  core::CCLCDomainReservationTable reservations_;
  SequenceSnapshotSource snapshots_;
  SetupCatalogSource catalogs_;
  SetupResolver resolver_;
  core::test::FakeCellAdmissionPort kernel_;
  core::ManualScheduler scheduler_;
  const std::vector<DomainSetup> &domains_;
  core::ComponentLifecycleCoordinator coordinator_;
};

[[nodiscard]] std::string Describe(const core::ComponentLifecycleStatusDTO &status) {
  return status.error ? status.error->code + ": " + status.error->message +
                            (status.error->details.contains("reason")
                                 ? " (" + status.error->details.at("reason") + ")"
                                 : std::string())
                      : std::string("no error");
}

// (a) After a Recover of a never-armed arm: the arm Declared, the gripper
// still Prepared, the target Disabled. The Disable switches off only the
// gripper; the arm has nothing engaged and no Disable step applies.
TEST(RealXArmCCLCLifecycle, DisableFromAnUnarmedArmSwitchesOffOnlyThePreparedGripper) {
  const std::vector<DomainSetup> domains{
      MakeDomain("lite6_robot.arm.control", "lite6_robot.ros2_control",
            Catalog(xarm_control::kXArmRos2ControlLifecycleProviderManifestId,
                    xarm_control::make_xarm_ros2_control_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(UnarmedArm(), std::map<std::string, Report>{})),
      MakeDomain("lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
            Catalog(xarm_control::kXArmRos2ControlGripperLifecycleProviderManifestId,
                    retained_ros2_control_gripper_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(
                Gripper(State::kPrepared),
                std::map<std::string, Report>{
                    {"ros2_control.controller.deactivate", Gripper(State::kDisabled)}}))};

  RealCellLifecycle cell(domains);
  const auto status = cell.Run(Intent::kDisable, State::kDeclared, 3U, "disable.1");
  ASSERT_TRUE(status);
  EXPECT_EQ(status->terminal_result, Terminal::kSucceeded) << Describe(*status);
  EXPECT_EQ(status->current_state, State::kDisabled);
  ASSERT_EQ(status->domains.size(), 2U);
  EXPECT_EQ(status->domains[0].current_state, State::kDisabled);
  EXPECT_EQ(status->domains[1].current_state, State::kDisabled);
  // The arm's Disable is already done: no transition dispatched.
  EXPECT_FALSE(domains[0].port->last());
  EXPECT_TRUE(domains[0].device->dispatched().empty());
  EXPECT_EQ(domains[1].device->dispatched(),
            std::vector<std::string>{"ros2_control.controller.deactivate"});
  EXPECT_EQ(Substeps(domains[1]),
            (std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>{
                {"ros2_control.controller.deactivate",
                 core::LifecycleTransitionOutcomeDTO::kSucceeded}}));
  EXPECT_EQ(cell.PersistedState(), State::kDisabled);
  EXPECT_TRUE(cell.Accepting());
}

[[nodiscard]] std::vector<DomainSetup> ArmAndPreparedGripper(Report arm) {
  return {MakeDomain("lite6_robot.arm.control", "lite6_robot.ros2_control",
                Catalog(xarm_control::kXArmRos2ControlLifecycleProviderManifestId,
                        xarm_control::make_xarm_ros2_control_lifecycle_plans()),
                std::make_shared<ScriptedDevice>(std::move(arm),
                                                 std::map<std::string, Report>{})),
          MakeDomain("lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
                Catalog(xarm_control::kXArmRos2ControlGripperLifecycleProviderManifestId,
                        retained_ros2_control_gripper_lifecycle_plans()),
                std::make_shared<ScriptedDevice>(
                    Gripper(State::kPrepared),
                    std::map<std::string, Report>{
                        {"ros2_control.controller.deactivate", Gripper(State::kDisabled)}}))};
}

[[nodiscard]] core::Result<core::LifecycleDomainPlanDecision>
ArmDisableDecision(const Report &arm) {
  const auto domains = ArmAndPreparedGripper(arm);
  const auto occurrence = Occurrence(domains[0]);
  const auto observation = domains[0].port->Observe(
      {core::LifecycleTransactionId("lifecycle.1"), core::LifecycleTransactionGeneration(1U),
       core::ControllableComponentId("lite6_robot"), occurrence.control_domain_id,
       occurrence.control_domain_revision, occurrence.provider_instance_id,
       occurrence.provider_manifest_id, occurrence.provider_manifest_revision,
       occurrence.provider_runtime_unit_id, occurrence.provider_runtime_epoch});
  return core::EvaluateLifecycleDomainPlan(Intent::kDisable, State::kDisabled,
                                           domains[0].catalog, observation.value());
}

// The arm's own Recover postcondition (`recovered`: hardware and controller
// inactive, no vendor error or warning, no fault, transport ready) is the
// proof that a Declared arm has nothing engaged. Without it the Disable
// fails closed, as before.
TEST(RealXArmCCLCLifecycle, OnlyAProvenUnarmedDeclaredArmIsAlreadyDisabled) {
  const auto unarmed = ArmDisableDecision(UnarmedArm());
  ASSERT_TRUE(unarmed.ok()) << unarmed.error().code;
  EXPECT_EQ(unarmed.value().kind, core::LifecycleDomainPlanDecisionKind::kAlreadySatisfied);

  const auto engaged = ArmDisableDecision(ArmWith(
      {{"external.hardware_active", true}, {"external.controller_active", true}}));
  ASSERT_FALSE(engaged.ok());
  EXPECT_EQ(engaged.error().code, "component_lifecycle_transition_missing");

  const auto no_transport = ArmDisableDecision(ArmWith({{"external.transport_ready", false}}));
  ASSERT_FALSE(no_transport.ok());
  EXPECT_EQ(no_transport.error().code, "component_lifecycle_transition_missing");

  const auto vendor_error = ArmDisableDecision(ArmWith({{core::kDeviceErrorPresentFactId, true}}));
  EXPECT_FALSE(vendor_error.ok());
}

// Through the coordinator: a Declared arm with hardware and controller
// active is not recorded Disabled; the Disable fails typed before any
// dispatch (the port's admission: not ready), and the prepared gripper is
// not switched off either.
TEST(RealXArmCCLCLifecycle, DisableWithAnEngagedDeclaredArmFailsClosedWithoutDispatch) {
  const auto domains = ArmAndPreparedGripper(ArmWith(
      {{"external.hardware_active", true}, {"external.controller_active", true}}));
  RealCellLifecycle cell(domains);
  const auto status = cell.Run(Intent::kDisable, State::kDeclared, 3U, "disable.engaged");
  ASSERT_TRUE(status);
  EXPECT_EQ(status->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status->terminal_result, Terminal::kFailed);
  ASSERT_TRUE(status->error);
  EXPECT_EQ(status->error->code, "component_lifecycle_transition_missing");
  for (const auto &domain : status->domains)
    EXPECT_NE(domain.current_state, State::kDisabled);
  EXPECT_FALSE(domains[0].port->last());
  EXPECT_FALSE(domains[1].port->last());
  EXPECT_TRUE(domains[0].device->dispatched().empty());
  EXPECT_TRUE(domains[1].device->dispatched().empty());
  EXPECT_NE(cell.PersistedState(), State::kDisabled);
}

/// The persisting-error failure `VerifyClear` reports with the e-stop still
/// engaged: a known failure with no effect, the vendor code and the status.
[[nodiscard]] RetainedExternallyManagedPrimitiveOutcome ClearDidNotTakeEffect() {
  return {Outcome::kFailedNoEffect,
          core::ErrorDTO{"execution_rejected",
                         "the emergency stop is still engaged (C2): release it and "
                         "try again",
                         {{"reason", "xarm_error_persists_after_clear"},
                          {"vendor_code", "C2"},
                          {"vendor_status", "C2 mode=0 state=4"},
                          {"ros2_control_status",
                           "hardware=active controller=inactive broadcaster=inactive"}}}};
}

// M6 XADAPT (REALWEDGE, maintainer 2026-10-28): Recover cell with the
// emergency stop still pressed. The plugin left the hardware active under
// C2. The Recover clears the latched error first, before anything else; the
// error is still reported, so the clear had no effect and the Recover fails
// typed with the vendor code and the current status, having done nothing
// else: no disarm (V-B2: the runner stops at the first failed substep). Its
// claim is released and CCLC admission stays open. After the release the
// next Recover clears the error, then disarms.
TEST(RealXArmCCLCLifecycle, RecoverWithTheEmergencyStopEngagedFailsWithNoEffectThenRecovers) {
  auto faulted = ArmWith({{"external.hardware_active", true},
                          {core::kDeviceErrorPresentFactId, true}});
  faulted.state = State::kFaulted;
  auto domains = ArmAndPreparedGripper(faulted);
  const auto arm = domains[0].device;
  arm->SetResult(std::string(core::kDeviceClearErrorPrimitiveId),
                 ClearDidNotTakeEffect());

  RealCellLifecycle cell(domains);
  const auto failed = cell.Run(Intent::kRecover, State::kDegraded, 3U, "recover.estop");
  ASSERT_TRUE(failed);
  // The clear only: nothing was disarmed.
  EXPECT_EQ(arm->dispatched(),
            std::vector<std::string>{std::string(core::kDeviceClearErrorPrimitiveId)});
  EXPECT_EQ(failed->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed)
      << Describe(*failed);
  EXPECT_EQ(failed->terminal_result, Terminal::kFailed);
  EXPECT_EQ(Substeps(domains[0]),
            (std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>{
                {std::string(core::kDeviceClearErrorPrimitiveId),
                 core::LifecycleTransitionOutcomeDTO::kFailed}}));
  ASSERT_TRUE(failed->error);
  EXPECT_EQ(failed->error->details.at("reason"), "xarm_error_persists_after_clear");
  EXPECT_EQ(failed->error->details.at("vendor_code"), "C2");
  EXPECT_EQ(failed->error->details.at("vendor_status"), "C2 mode=0 state=4");
  EXPECT_EQ(failed->error->details.at("ros2_control_status"),
            "hardware=active controller=inactive broadcaster=inactive");
  ASSERT_EQ(failed->domains.size(), 2U);
  EXPECT_TRUE(domains[1].device->dispatched().empty());
  EXPECT_TRUE(cell.Accepting());

  // Released: the clear takes effect (still armed), then the disarm.
  auto cleared = ArmWith({{"external.hardware_active", true}});
  cleared.state = State::kDegraded;
  arm->SetResult(std::string(core::kDeviceClearErrorPrimitiveId), std::nullopt);
  arm->SetAfter(std::string(core::kDeviceClearErrorPrimitiveId), cleared);
  arm->SetAfter("ros2_control.hardware.deactivate", UnarmedArm());
  const auto recovered = cell.Run(Intent::kRecover, State::kDegraded, 4U, "recover.retry");
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->terminal_result, Terminal::kSucceeded) << Describe(*recovered);
  EXPECT_EQ(arm->dispatched(),
            (std::vector<std::string>{std::string(core::kDeviceClearErrorPrimitiveId),
                                      std::string(core::kDeviceClearErrorPrimitiveId),
                                      "ros2_control.hardware.deactivate"}));
  EXPECT_EQ(Substeps(domains[0]),
            (std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>{
                {std::string(core::kDeviceClearErrorPrimitiveId),
                 core::LifecycleTransitionOutcomeDTO::kSucceeded},
                {"ros2_control.hardware.deactivate",
                 core::LifecycleTransitionOutcomeDTO::kSucceeded}}));
  EXPECT_TRUE(domains[1].device->dispatched().empty());
  EXPECT_TRUE(cell.Accepting());
}

// M6 XADAPT: a Prepare clears the vendor error, then the controller manager
// refuses the hardware activation. The handle reads the next vendor report
// and the controller activity: unchanged is a failure with no effect,
// changed one with an observed effect, a distinction only in the handle and
// the error's details. Either way (V-B2, case 7a) the transaction fails
// with the typed refusal and the current status, the clear listed as done,
// the state recorded from the fresh observation after the failure (VB2-O1:
// the unarmed resting arm, Declared), and admission stays open.
TEST(RealXArmCCLCLifecycle, PrepareWithARefusedHardwareActivationEndsDefinite) {
  for (const auto refused : {Outcome::kFailedNoEffect, Outcome::kFailedEffectObserved}) {
    auto domains = ArmAndPreparedGripper(ArmWith({{core::kDeviceErrorPresentFactId, true}}));
    const auto arm = domains[0].device;
    arm->SetAfter(std::string(core::kDeviceClearErrorPrimitiveId), UnarmedArm());
    arm->SetResult("ros2_control.hardware.activate",
                   RetainedExternallyManagedPrimitiveOutcome{
                       refused,
                       core::ErrorDTO{"execution_rejected",
                                      "the controller manager refused the "
                                      "hardware transition",
                                      {{"reason", "ros2_control_hardware_activation_refused"},
                                       {"vendor_status", "mode=0 state=4"},
                                       {"ros2_control_status",
                                        "hardware=inactive controller=inactive "
                                        "broadcaster=active"}}}});

    RealCellLifecycle cell(domains);
    const auto failed = cell.Run(Intent::kPrepare, State::kDisabled, 3U, "prepare.refused");
    ASSERT_TRUE(failed);
    EXPECT_EQ(arm->dispatched(),
              (std::vector<std::string>{std::string(core::kDeviceClearErrorPrimitiveId),
                                        "ros2_control.hardware.activate"}));
    EXPECT_EQ(failed->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed)
        << Describe(*failed);
    EXPECT_EQ(failed->terminal_result, Terminal::kFailed);
    EXPECT_EQ(Substeps(domains[0]),
              (std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>{
                  {std::string(core::kDeviceClearErrorPrimitiveId),
                   core::LifecycleTransitionOutcomeDTO::kSucceeded},
                  {"ros2_control.hardware.activate",
                   core::LifecycleTransitionOutcomeDTO::kFailed}}));
    ASSERT_TRUE(failed->error);
    EXPECT_EQ(failed->error->details.at("reason"), "ros2_control_hardware_activation_refused");
    EXPECT_EQ(failed->error->details.at("ros2_control_status"),
              "hardware=inactive controller=inactive broadcaster=active");
    EXPECT_TRUE(failed->domains[0].observed_after);
    EXPECT_TRUE(domains[1].device->dispatched().empty());
    EXPECT_TRUE(cell.Accepting());
    EXPECT_EQ(failed->domains[0].current_state, State::kDeclared);
    EXPECT_NE(cell.PersistedState(), State::kPrepared);
  }
}

/// What the handle reports once a call or status query got no answer in
/// time: the unresponsive fault (`external.fault_present`, Faulted; the
/// target driver opens the incident from that `fault` cause).
[[nodiscard]] Report Unresponsive(Report report) {
  report.state = State::kFaulted;
  for (auto &fact : report.facts)
    if (fact.lifecycle_fact_id.value() == "external.fault_present")
      fact.value.boolean_value = true;
  return report;
}

/// What the handle returns for a call with no answer in time (SX-661): a
/// known failure whose observed effect is the fault it raises, with the
/// typed code and the queried status. The port maps it by its code to a
/// lost connection (V-B2 §2.2, D8).
[[nodiscard]] RetainedExternallyManagedPrimitiveOutcome NoAnswer(std::string code,
                                                                 std::string reason) {
  return {Outcome::kFailedEffectObserved,
          core::ErrorDTO{std::move(code), "no answer in time",
                         {{"reason", std::move(reason)},
                          {"vendor_status", "unavailable"},
                          {"ros2_control_status", "unavailable"}}}};
}

// M6 XADAPT (maintainer: no hang; unresponsiveness goes to CCLC): Prepare
// clears the error, then the controller manager does not answer the
// hardware activation within its 4 s bound. The handle reports the
// unresponsive component fault, so the transaction ends in the same run as
// a lost connection (V-B2) with the typed code and status as its result
// (SX-661): no stall, admission open. The operator's Recover then
// re-queries the manager (`ros2_control.controller.recover`) and ends it.
TEST(RealXArmCCLCLifecycle, ControllerManagerUnresponsiveDuringPrepareFailsAtOnceAsAFault) {
  auto domains = ArmAndPreparedGripper(ArmWith({{core::kDeviceErrorPresentFactId, true}}));
  const auto arm = domains[0].device;
  arm->SetAfter(std::string(core::kDeviceClearErrorPrimitiveId), UnarmedArm());
  arm->SetAfter("ros2_control.hardware.activate", Unresponsive(UnarmedArm()));
  arm->SetResult("ros2_control.hardware.activate",
                 NoAnswer("controller_manager_unresponsive",
                          "ros2_control_hardware_state_timeout"));

  RealCellLifecycle cell(domains);
  const auto failed = cell.Run(Intent::kPrepare, State::kDisabled, 3U, "prepare.unresponsive");
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed)
      << Describe(*failed);
  EXPECT_EQ(failed->terminal_result, Terminal::kConnectionLost);
  EXPECT_FALSE(failed->reconciliation_required);
  EXPECT_EQ(Substeps(domains[0]),
            (std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>{
                {std::string(core::kDeviceClearErrorPrimitiveId),
                 core::LifecycleTransitionOutcomeDTO::kSucceeded},
                {"ros2_control.hardware.activate",
                 core::LifecycleTransitionOutcomeDTO::kConnectionLost}}));
  // SX-661: the typed code and status are the transaction's result.
  ASSERT_TRUE(failed->error);
  EXPECT_EQ(failed->error->code, "controller_manager_unresponsive");
  EXPECT_EQ(failed->error->details.at("reason"), "ros2_control_hardware_state_timeout");
  EXPECT_EQ(failed->error->details.at("ros2_control_status"), "unavailable");
  EXPECT_EQ(arm->dispatched(),
            (std::vector<std::string>{std::string(core::kDeviceClearErrorPrimitiveId),
                                      "ros2_control.hardware.activate"}));
  EXPECT_TRUE(domains[1].device->dispatched().empty());
  EXPECT_TRUE(cell.Accepting());
  EXPECT_NE(cell.PersistedState(), State::kPrepared);

  // Other work is accepted: the operator's Recover re-queries the manager;
  // answered, the fault ends.
  arm->SetAfter("ros2_control.controller.recover", UnarmedArm());
  const auto recovered = cell.Run(Intent::kRecover, State::kDegraded, 4U, "recover.answered");
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->terminal_result, Terminal::kSucceeded) << Describe(*recovered);
  EXPECT_EQ(arm->dispatched().back(), "ros2_control.controller.recover");
  EXPECT_TRUE(cell.Accepting());
}

// M6 XADAPT: Recover cell with the robot's controller silent: the clear
// (the first step) gets no answer within its 4 s bound
// (`robot_controller_unresponsive`), and the robot's status goes stale. The
// fault ends the Recover at once as a lost connection (V-B2), nothing else
// dispatched, admission open. With the robot answering again the next
// Recover runs.
TEST(RealXArmCCLCLifecycle, RobotUnresponsiveDuringRecoverFailsAtOnceAsAFault) {
  auto faulted = ArmWith({{"external.hardware_active", true},
                          {"external.fault_present", true},
                          {core::kDeviceErrorPresentFactId, true}});
  faulted.state = State::kFaulted;
  auto domains = ArmAndPreparedGripper(faulted);
  const auto arm = domains[0].device;
  arm->SetAfter(std::string(core::kDeviceClearErrorPrimitiveId),
                WithUnknown(Unresponsive(faulted),
                            {core::kDeviceAcceptsCommandsFactId,
                             core::kDeviceErrorPresentFactId,
                             core::kDeviceWarningPresentFactId}));
  arm->SetResult(std::string(core::kDeviceClearErrorPrimitiveId),
                 NoAnswer("robot_controller_unresponsive", "xarm_service_timeout"));

  RealCellLifecycle cell(domains);
  const auto failed = cell.Run(Intent::kRecover, State::kDegraded, 3U, "recover.silent");
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed)
      << Describe(*failed);
  EXPECT_EQ(failed->terminal_result, Terminal::kConnectionLost);
  EXPECT_FALSE(failed->reconciliation_required);
  ASSERT_TRUE(failed->error);
  EXPECT_EQ(failed->error->code, "robot_controller_unresponsive");
  EXPECT_EQ(failed->error->details.at("reason"), "xarm_service_timeout");
  EXPECT_EQ(arm->dispatched(),
            std::vector<std::string>{std::string(core::kDeviceClearErrorPrimitiveId)});
  EXPECT_EQ(Substeps(domains[0]),
            (std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>{
                {std::string(core::kDeviceClearErrorPrimitiveId),
                 core::LifecycleTransitionOutcomeDTO::kConnectionLost}}));
  EXPECT_TRUE(domains[1].device->dispatched().empty());
  EXPECT_TRUE(cell.Accepting());

  // The robot answers again (the e-stop released): the next Recover clears
  // the error, then disarms.
  arm->SetCurrent(faulted);
  arm->SetResult(std::string(core::kDeviceClearErrorPrimitiveId), std::nullopt);
  auto cleared = ArmWith({{"external.hardware_active", true}});
  cleared.state = State::kDegraded;
  arm->SetAfter(std::string(core::kDeviceClearErrorPrimitiveId), cleared);
  arm->SetAfter("ros2_control.hardware.deactivate", UnarmedArm());
  const auto recovered = cell.Run(Intent::kRecover, State::kDegraded, 4U, "recover.back");
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->terminal_result, Terminal::kSucceeded) << Describe(*recovered);
  EXPECT_TRUE(cell.Accepting());
}

// M6 XADAPT (SX-664), V-B2 §6.2: Prepare clears the error, its activation
// is refused, and the owner's cancel cuts the handle's second status query
// short: an uncertain step, with unchanged facts. With no reconciliation in
// CCLC the transition fails with the step's typed reason, the clear listed
// as done; CCLC records a fresh observation after it. This is the
// regression test of the REALWEDGE stall: no stall, admission open.
TEST(RealXArmCCLCLifecycle, CancelledProofQueryAfterAnObservedStepNeverStalls) {
  auto domains = ArmAndPreparedGripper(ArmWith({{core::kDeviceErrorPresentFactId, true}}));
  const auto arm = domains[0].device;
  arm->SetAfter(std::string(core::kDeviceClearErrorPrimitiveId), UnarmedArm());
  arm->SetResult("ros2_control.hardware.activate",
                 RetainedExternallyManagedPrimitiveOutcome{
                     Outcome::kUncertain,
                     core::ErrorDTO{"execution_rejected",
                                    "the step's status query was cancelled; its "
                                    "effect is unknown",
                                    {{"reason", "ros2_control_status_query_cancelled"},
                                     {"step_reason",
                                      "ros2_control_hardware_activation_refused"}}}});

  RealCellLifecycle cell(domains);
  const auto failed = cell.Run(Intent::kPrepare, State::kDisabled, 3U, "prepare.cancelled");
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed)
      << Describe(*failed);
  EXPECT_EQ(failed->terminal_result, Terminal::kFailed);
  ASSERT_TRUE(failed->error);
  EXPECT_EQ(failed->error->details.at("reason"), "ros2_control_status_query_cancelled");
  EXPECT_EQ(Substeps(domains[0]),
            (std::vector<std::pair<std::string, core::LifecycleTransitionOutcomeDTO>>{
                {std::string(core::kDeviceClearErrorPrimitiveId),
                 core::LifecycleTransitionOutcomeDTO::kSucceeded},
                {"ros2_control.hardware.activate",
                 core::LifecycleTransitionOutcomeDTO::kFailed}}));
  ASSERT_EQ(failed->domains.size(), 2U);
  EXPECT_TRUE(failed->domains[0].observed_after);
  EXPECT_FALSE(failed->reconciliation_required);
  EXPECT_TRUE(cell.Accepting());
}

// (b) Shared ros2_control plans (Mock/Isaac; the Real xArm arm never ends a
// Recover Active): all domains already Active at a Recover. It records them
// Active and the component Prepared; the following Enable is already
// satisfied in every domain and reaches Active with no transition error.
TEST(RealXArmCCLCLifecycle, AllActiveRecoverRecordsPreparedThenEnableReachesActive) {
  // The shared plans on both domains, as on Mock and Isaac.
  const std::vector<DomainSetup> shared{
      MakeDomain("lite6_robot.arm.control", "lite6_robot.ros2_control",
            Catalog(kRos2ControlLifecycleProviderManifestId,
                    retained_ros2_control_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(Gripper(State::kActive),
                                             std::map<std::string, Report>{})),
      MakeDomain("lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
            Catalog(kRos2ControlLifecycleProviderManifestId,
                    retained_ros2_control_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(Gripper(State::kActive),
                                             std::map<std::string, Report>{}))};

  RealCellLifecycle cell(shared);
  const auto recovered = cell.Run(Intent::kRecover, State::kDegraded, 3U, "recover.1");
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->terminal_result, Terminal::kSucceeded) << Describe(*recovered);
  for (const auto &domain : recovered->domains)
    EXPECT_EQ(domain.current_state, State::kActive);
  EXPECT_EQ(recovered->current_state, State::kPrepared);
  EXPECT_EQ(cell.PersistedState(), State::kPrepared);

  const auto enabled = cell.Run(Intent::kEnable, State::kPrepared, 4U, "enable.1");
  ASSERT_TRUE(enabled);
  EXPECT_EQ(enabled->terminal_result, Terminal::kSucceeded) << Describe(*enabled);
  EXPECT_EQ(enabled->current_state, State::kActive);
  for (const auto &domain : enabled->domains)
    EXPECT_EQ(domain.current_state, State::kActive);
  EXPECT_EQ(cell.PersistedState(), State::kActive);
  EXPECT_TRUE(shared[0].device->dispatched().empty());
  EXPECT_TRUE(shared[1].device->dispatched().empty());
  EXPECT_TRUE(cell.Accepting());
}

/// A shared-plan domain with its trajectory controller loaded but inactive,
/// before Prepare: `ros2_control.controller.prepare` is eligible.
[[nodiscard]] Report LoadedDisabled() {
  return Sorted({State::kDisabled,
                 {Fact("external.controller_configuration_ready", false),
                  Fact("external.controller_loaded", true),
                  Fact("external.fault_present", false),
                  Fact("external.transport_ready", true)}});
}

// (c) M6, shared ros2_control plans (Mock/Isaac): a Prepare that finds a
// domain Active succeeds. One domain is Active after its prepare step (the
// post-step observation), the other already at the start. Both record
// Active; the component records Prepared (the lowest, never command-ready);
// the following Enable completes with no dispatch.
TEST(RealXArmCCLCLifecycle, PrepareThatFindsActiveSucceedsThenEnableDispatchesNothing) {
  const std::vector<DomainSetup> shared{
      MakeDomain("lite6_robot.arm.control", "lite6_robot.ros2_control",
            Catalog(kRos2ControlLifecycleProviderManifestId,
                    retained_ros2_control_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(
                LoadedDisabled(),
                std::map<std::string, Report>{
                    {"ros2_control.controller.prepare", Gripper(State::kActive)}})),
      MakeDomain("lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
            Catalog(kRos2ControlLifecycleProviderManifestId,
                    retained_ros2_control_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(Gripper(State::kActive),
                                             std::map<std::string, Report>{}))};

  RealCellLifecycle cell(shared);
  const auto prepared = cell.Run(Intent::kPrepare, State::kDisabled, 3U, "prepare.1");
  ASSERT_TRUE(prepared);
  EXPECT_EQ(prepared->terminal_result, Terminal::kSucceeded) << Describe(*prepared);
  ASSERT_EQ(prepared->domains.size(), 2U);
  EXPECT_EQ(prepared->domains[0].current_state, State::kActive);
  EXPECT_EQ(prepared->domains[1].current_state, State::kActive);
  EXPECT_EQ(prepared->current_state, State::kPrepared);
  EXPECT_EQ(cell.PersistedState(), State::kPrepared);
  EXPECT_EQ(shared[0].device->dispatched(),
            std::vector<std::string>{"ros2_control.controller.prepare"});
  EXPECT_FALSE(shared[1].port->last());
  EXPECT_TRUE(shared[1].device->dispatched().empty());

  const auto enabled = cell.Run(Intent::kEnable, State::kPrepared, 4U, "enable.1");
  ASSERT_TRUE(enabled);
  EXPECT_EQ(enabled->terminal_result, Terminal::kSucceeded) << Describe(*enabled);
  EXPECT_EQ(enabled->current_state, State::kActive);
  EXPECT_EQ(cell.PersistedState(), State::kActive);
  EXPECT_EQ(shared[0].device->dispatched(),
            std::vector<std::string>{"ros2_control.controller.prepare"});
  EXPECT_TRUE(shared[1].device->dispatched().empty());
  EXPECT_TRUE(cell.Accepting());
}

// A faulted Active domain is not "found Active": the Prepare postcondition
// requires no fault and no step applies, so the Prepare fails before any
// dispatch and nothing is recorded Prepared.
TEST(RealXArmCCLCLifecycle, PrepareDoesNotAcceptAFaultedActiveDomain) {
  auto faulted = Gripper(State::kActive);
  for (auto &fact : faulted.facts)
    if (fact.lifecycle_fact_id.value() == "external.fault_present") fact.value.boolean_value = true;
  const std::vector<DomainSetup> shared{
      MakeDomain("lite6_robot.arm.control", "lite6_robot.ros2_control",
            Catalog(kRos2ControlLifecycleProviderManifestId,
                    retained_ros2_control_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(faulted, std::map<std::string, Report>{})),
      MakeDomain("lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
            Catalog(kRos2ControlLifecycleProviderManifestId,
                    retained_ros2_control_lifecycle_plans()),
            std::make_shared<ScriptedDevice>(Gripper(State::kActive),
                                             std::map<std::string, Report>{}))};

  RealCellLifecycle cell(shared);
  const auto status = cell.Run(Intent::kPrepare, State::kDisabled, 3U, "prepare.faulted");
  ASSERT_TRUE(status);
  EXPECT_EQ(status->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed);
  ASSERT_TRUE(status->error);
  EXPECT_EQ(status->error->code, "component_lifecycle_transition_missing");
  EXPECT_NE(cell.PersistedState(), State::kPrepared);
  EXPECT_TRUE(shared[0].device->dispatched().empty());
  EXPECT_TRUE(shared[1].device->dispatched().empty());
}

} // namespace
} // namespace manyforge::roboplan_backend
