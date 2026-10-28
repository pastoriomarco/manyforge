// End-to-end CCLC lifecycle transactions on the maintained Real xArm and
// shared ros2_control plans: the coordinator, its SQLite journal and the
// plan evaluator, with scripted provider observations (REC FIX5).
#include "../../manyforge_core/test/cclc_test_admission_port.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/cclc_domain_reservation_table.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <manyforge_xarm_control/lifecycle_profile.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
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

/// A provider port with one current report; a dispatched primitive moves it
/// to that primitive's scripted result.
class ScriptedPort final : public core::IBrandLifecyclePort {
public:
  ScriptedPort(Report current, std::map<std::string, Report> after)
      : current_(std::move(current)), after_(std::move(after)) {}

  core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override {
    std::lock_guard lock(mutex_);
    ++generation_;
    return core::Result<core::BrandLifecycleObservationDTO>::Ok(
        {query.lifecycle_transaction_id, query.lifecycle_transaction_generation,
         query.controllable_component_id, query.control_domain_id,
         query.control_domain_revision, query.provider_instance_id,
         query.provider_manifest_id, query.provider_manifest_revision,
         query.provider_runtime_unit_id, query.provider_runtime_epoch,
         generation_, generation_, 50, 90, 2'000'000, current_.state,
         current_.facts});
  }

  core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token) override {
    std::lock_guard lock(mutex_);
    dispatched_.push_back(intent.lifecycle_primitive_id.value());
    const auto found = after_.find(intent.lifecycle_primitive_id.value());
    if (found != after_.end()) current_ = found->second;
    return {intent.lifecycle_transaction_id,
            intent.lifecycle_transaction_generation,
            intent.controllable_component_id,
            intent.control_domain_id,
            intent.control_domain_revision,
            intent.provider_instance_id,
            intent.provider_manifest_id,
            intent.provider_manifest_revision,
            intent.provider_runtime_unit_id,
            intent.provider_runtime_epoch,
            intent.lifecycle_primitive_id,
            intent.lifecycle_primitive_attempt_id,
            intent.expected_observation_generation,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown,
            core::LifecyclePrimitiveEffectDTO::kObserved,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded,
            std::nullopt};
  }

  [[nodiscard]] std::vector<std::string> dispatched() const {
    std::lock_guard lock(mutex_);
    return dispatched_;
  }

private:
  mutable std::mutex mutex_;
  Report current_;
  std::map<std::string, Report> after_;
  std::vector<std::string> dispatched_;
  std::uint64_t generation_ = 0U;
};

/// The two domains of one Real Lite 6 component, arm before gripper.
struct DomainSetup final {
  std::string domain;
  std::string instance;
  core::BrandLifecyclePlanCatalogDTO catalog;
  std::shared_ptr<ScriptedPort> port;
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
Occurrence(const DomainSetup &setup) {
  return {core::ControlDomainId(setup.domain),
          core::ControlDomainRevision("domain-revision"),
          core::ProviderInstanceId(setup.instance),
          setup.catalog.provider_manifest_id,
          setup.catalog.provider_manifest_revision,
          core::RuntimeUnitId("robot_cell.ros2_control_execution"),
          core::RuntimeEpoch("runtime-epoch.1"),
          setup.catalog.catalog_digest};
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
                     kernel_, clock_, scheduler_) {
    clock_.set(100);
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
  core::FakeClock clock_;
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
      {"lite6_robot.arm.control", "lite6_robot.ros2_control",
       Catalog(xarm_control::kXArmRos2ControlLifecycleProviderManifestId,
               xarm_control::make_xarm_ros2_control_lifecycle_plans()),
       std::make_shared<ScriptedPort>(UnarmedArm(), std::map<std::string, Report>{})},
      {"lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
       Catalog(kXArmRos2ControlGripperLifecycleProviderManifestId,
               retained_ros2_control_gripper_lifecycle_plans()),
       std::make_shared<ScriptedPort>(
           Gripper(State::kPrepared),
           std::map<std::string, Report>{
               {"ros2_control.controller.deactivate", Gripper(State::kDisabled)}})}};

  RealCellLifecycle cell(domains);
  const auto status = cell.Run(Intent::kDisable, State::kDeclared, 3U, "disable.1");
  ASSERT_TRUE(status);
  EXPECT_EQ(status->terminal_result, core::ComponentLifecycleTerminalResultDTO::kSucceeded)
      << Describe(*status);
  EXPECT_EQ(status->current_state, State::kDisabled);
  ASSERT_EQ(status->domains.size(), 2U);
  EXPECT_EQ(status->domains[0].current_state, State::kDisabled);
  EXPECT_EQ(status->domains[0].effect, core::ComponentLifecycleDomainEffectDTO::kKnownNoEffect);
  EXPECT_EQ(status->domains[1].current_state, State::kDisabled);
  EXPECT_EQ(status->domains[1].effect, core::ComponentLifecycleDomainEffectDTO::kEffectObserved);
  EXPECT_TRUE(domains[0].port->dispatched().empty());
  EXPECT_EQ(domains[1].port->dispatched(),
            std::vector<std::string>{"ros2_control.controller.deactivate"});
  EXPECT_EQ(cell.PersistedState(), State::kDisabled);
  EXPECT_TRUE(cell.Accepting());
}

[[nodiscard]] std::vector<DomainSetup> ArmAndPreparedGripper(Report arm) {
  return {{"lite6_robot.arm.control", "lite6_robot.ros2_control",
           Catalog(xarm_control::kXArmRos2ControlLifecycleProviderManifestId,
                   xarm_control::make_xarm_ros2_control_lifecycle_plans()),
           std::make_shared<ScriptedPort>(std::move(arm), std::map<std::string, Report>{})},
          {"lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
           Catalog(kXArmRos2ControlGripperLifecycleProviderManifestId,
                   retained_ros2_control_gripper_lifecycle_plans()),
           std::make_shared<ScriptedPort>(
               Gripper(State::kPrepared),
               std::map<std::string, Report>{
                   {"ros2_control.controller.deactivate", Gripper(State::kDisabled)}})}};
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
// dispatch, and the prepared gripper is not switched off either.
TEST(RealXArmCCLCLifecycle, DisableWithAnEngagedDeclaredArmFailsClosedWithoutDispatch) {
  const auto domains = ArmAndPreparedGripper(ArmWith(
      {{"external.hardware_active", true}, {"external.controller_active", true}}));
  RealCellLifecycle cell(domains);
  const auto status = cell.Run(Intent::kDisable, State::kDeclared, 3U, "disable.engaged");
  ASSERT_TRUE(status);
  EXPECT_EQ(status->phase, core::ComponentLifecycleTransactionPhaseDTO::kFailed);
  EXPECT_EQ(status->terminal_result,
            core::ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect);
  ASSERT_TRUE(status->error);
  EXPECT_EQ(status->error->code, "component_lifecycle_transition_missing");
  for (const auto &domain : status->domains)
    EXPECT_NE(domain.current_state, State::kDisabled);
  EXPECT_TRUE(domains[0].port->dispatched().empty());
  EXPECT_TRUE(domains[1].port->dispatched().empty());
  EXPECT_NE(cell.PersistedState(), State::kDisabled);
}

// (b) Shared ros2_control plans (Mock/Isaac; the Real xArm arm never ends a
// Recover Active): all domains already Active at a Recover. It records them
// Active and the component Prepared; the following Enable is already
// satisfied in every domain and reaches Active with no transition error.
TEST(RealXArmCCLCLifecycle, AllActiveRecoverRecordsPreparedThenEnableReachesActive) {
  // The shared plans on both domains, as on Mock and Isaac.
  const std::vector<DomainSetup> shared{
      {"lite6_robot.arm.control", "lite6_robot.ros2_control",
       Catalog(kRos2ControlLifecycleProviderManifestId,
               retained_ros2_control_lifecycle_plans()),
       std::make_shared<ScriptedPort>(Gripper(State::kActive),
                                      std::map<std::string, Report>{})},
      {"lite6_robot.gripper.control", "lite6_robot.ros2_control.gripper",
       Catalog(kRos2ControlLifecycleProviderManifestId,
               retained_ros2_control_lifecycle_plans()),
       std::make_shared<ScriptedPort>(Gripper(State::kActive),
                                      std::map<std::string, Report>{})}};

  RealCellLifecycle cell(shared);
  const auto recovered = cell.Run(Intent::kRecover, State::kDegraded, 3U, "recover.1");
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->terminal_result, core::ComponentLifecycleTerminalResultDTO::kSucceeded)
      << Describe(*recovered);
  for (const auto &domain : recovered->domains)
    EXPECT_EQ(domain.current_state, State::kActive);
  EXPECT_EQ(recovered->current_state, State::kPrepared);
  EXPECT_EQ(cell.PersistedState(), State::kPrepared);

  const auto enabled = cell.Run(Intent::kEnable, State::kPrepared, 4U, "enable.1");
  ASSERT_TRUE(enabled);
  EXPECT_EQ(enabled->terminal_result, core::ComponentLifecycleTerminalResultDTO::kSucceeded)
      << Describe(*enabled);
  EXPECT_EQ(enabled->current_state, State::kActive);
  for (const auto &domain : enabled->domains)
    EXPECT_EQ(domain.current_state, State::kActive);
  EXPECT_EQ(cell.PersistedState(), State::kActive);
  EXPECT_TRUE(shared[0].port->dispatched().empty());
  EXPECT_TRUE(shared[1].port->dispatched().empty());
  EXPECT_TRUE(cell.Accepting());
}

} // namespace
} // namespace manyforge::roboplan_backend
