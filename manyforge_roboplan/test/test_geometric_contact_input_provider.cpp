#include <manyforge_roboplan/geometric_contact_input_provider.hpp>

#include <gtest/gtest.h>
#include <manyforge_core/io_signal_ingress.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <algorithm>
#include <condition_variable>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {
const core::InterruptSourceId kSource("geometric.contact");
const core::IoSourceIncarnation kIncarnation("activation.1");
const core::IoSignalId kSignal{core::ControllableComponentId("sensor.contact"),
                               "contact"};
const core::CollisionBodyId kEndpoint(
    "component/robot.arm/model/arm/body/tool/collision/0");
const core::CollisionBodyId kRobotBody(
    "component/robot.arm/model/arm/body/flange/collision/0");
const core::CollisionBodyId kTarget("scene/fixture.target/shape/0");

core::IoSignalDescriptorDTO descriptor(core::Duration age = 25) {
  return {kSignal, kSource, core::IoSignalValueType::kBoolean,
          core::IoSignalDirection::kInput, core::IoSignalUpdateMode::kSampled,
          core::IoTransitionCapability::kSampledOnly,
          {core::IoFreshnessMode::kSampleAge, age}, std::nullopt,
          core::IoSignalLivenessMode::kPeriodic};
}

core::CollisionPairId pair(const core::CollisionBodyId &endpoint = kEndpoint) {
  return kTarget < endpoint ? core::CollisionPairId{kTarget, endpoint}
                            : core::CollisionPairId{endpoint, kTarget};
}

class Publisher final : public core::ICollisionWorldSnapshotPublisher {
public:
  explicit Publisher(core::RuntimeCollisionQueryBundlePtr value)
      : value_(std::move(value)) {}
  core::CollisionWorldSnapshotPtr snapshot() const override {
    return value_->world;
  }
  core::RuntimeCollisionQueryBundlePtr
  published_runtime_collision_query_bundle() const override {
    return value_;
  }
private:
  core::RuntimeCollisionQueryBundlePtr value_;
};

class Engine final : public core::ICollisionQueryEngine {
public:
  explicit Engine(const core::CollisionBodyId &endpoint = kEndpoint) {
    capability_.runtime_query_max_joints = 8;
    capability_.runtime_query_max_world_bodies = 8;
    capability_.runtime_query_max_collision_pairs = 16;
    pair_ = pair(endpoint);
    observation_.pair = &pair_;
  }
  const core::CollisionEngineCapabilityDescriptor &capability() const override {
    return capability_;
  }
  core::CollisionResult check(const core::CollisionWorldSnapshot &,
                              const core::CollisionQuery &,
                              const core::CollisionPolicySnapshot &,
                              const core::CollisionCancellationToken &) override {
    return {};
  }
  core::RuntimeCollisionQueryPrepareStatus prepare_runtime_query(
      std::span<const std::string> names) noexcept override {
    prepared_names.assign(names.begin(), names.end());
    return core::RuntimeCollisionQueryPrepareStatus::kReady;
  }
  core::RuntimeCollisionQueryPrepareStatus prepare_runtime_snapshot(
      const core::CollisionWorldSnapshot &) noexcept override {
    return snapshot_ready ? core::RuntimeCollisionQueryPrepareStatus::kReady
                          : core::RuntimeCollisionQueryPrepareStatus::kEngineError;
  }
  core::RuntimeCollisionQueryResultView check_runtime(
      const core::CollisionWorldSnapshot &,
      const core::RuntimeCollisionQueryRequest &request) noexcept override {
    positions.assign(request.positions_rad.begin(), request.positions_rad.end());
    state_time = request.state_time_ns;
    ++queries;
    {
      std::unique_lock lock(block_mutex);
      entered = true;
      block_cv.notify_all();
      block_cv.wait(lock, [&] { return !block; });
    }
    if (clock && duration > 0)
      clock->advance(duration);
    return {status,
            failure,
            failure == core::RuntimeCollisionQueryFailureReason::kNone
                ? core::CollisionEngineStatus::kOk
                : core::CollisionEngineStatus::kDiagnosticIncomplete,
            duration,
            status == core::RuntimeCollisionQueryStatus::kCollision
                ? std::span<const core::RuntimeCollisionPairObservation>(
                      &observation_, 1)
                : std::span<const core::RuntimeCollisionPairObservation>{},
            {}};
  }
  std::string engine_id() const override { return "test"; }
  std::string engine_version() const override { return "1"; }

  core::CollisionEngineCapabilityDescriptor capability_;
  core::RuntimeCollisionQueryStatus status{
      core::RuntimeCollisionQueryStatus::kCollision};
  core::RuntimeCollisionQueryFailureReason failure{
      core::RuntimeCollisionQueryFailureReason::kNone};
  bool snapshot_ready{true};
  core::FakeClock *clock{};
  core::Duration duration{};
  std::vector<std::string> prepared_names;
  std::vector<double> positions;
  core::TimePoint state_time{};
  std::size_t queries{};
  std::mutex block_mutex;
  std::condition_variable block_cv;
  bool block{};
  bool entered{};
private:
  core::CollisionPairId pair_;
  core::RuntimeCollisionPairObservation observation_;
};

class GeometricContactInputProviderTest : public ::testing::Test {
protected:
  void Build(bool gripper_sample = true, std::size_t depth = 8,
             core::Duration age = 25) {
    clock.set(100);
    motion = publication("arm_joint", "motion", 90, 0.4);
    gripper = publication("finger_joint", "gripper", 95, 0.2,
                          gripper_sample);
    publisher_ = make_publisher(kEndpoint);
    engine = std::make_shared<Engine>();
    auto subsystem = core::IoSignalSubsystem::Create(
        {core::RequestOwnerEpoch("host"), 1, {descriptor(age)}, {}},
        {1, 1, depth, 4, 128, 1024}, clock);
    ASSERT_TRUE(subsystem.ok()) << subsystem.error().message;
    signals = std::shared_ptr<core::IoSignalSubsystem>(
        std::move(subsystem.value()));
    ASSERT_TRUE(signals->InstallSource(kSource, kIncarnation, std::nullopt).ok());
    auto queued = core::IoSignalIngress::Create(signals);
    ASSERT_TRUE(queued.ok());
    ingress = std::shared_ptr<core::IoSignalIngress>(std::move(queued.value()));
    auto binding = make_binding(kIncarnation, kEndpoint, publisher_, engine,
                                {motion, gripper}, age);
    binding.read_contact_bodies = [this] {
      if (selection_error) return core::Result<std::vector<core::CollisionBodyId>>::Err(*selection_error);
      return core::Result<std::vector<core::CollisionBodyId>>::Ok(selected_bodies);
    };
    auto made = GeometricContactInputProvider::Create(
        ingress, clock, scheduler, std::move(binding), 1U);
    ASSERT_TRUE(made.ok()) << made.error().message;
    provider = std::move(made.value());
  }

  std::shared_ptr<core::ObservedRobotStatePublication>
  publication(std::string joint, std::string source, core::TimePoint at,
              double position, bool publish = true) {
    auto result = std::make_shared<core::ObservedRobotStatePublication>(
        std::vector<std::string>{joint},
        std::vector<core::ObservedRobotStateSourceConfig>{{
            core::ObservedRobotStateSourceKind::kJointState, std::move(source),
            core::ObservedRobotStateAuthorityProfile::kMeasured, 50}});
    if (publish) {
      const std::vector<std::string> names{joint};
      const std::vector<double> positions{position};
      const std::vector<double> velocities{0.0};
      EXPECT_TRUE(result->publish(core::ObservedRobotStateSourceKind::kJointState,
                                  names, positions, velocities, at, at)
                      .published);
    }
    return result;
  }

  std::shared_ptr<Publisher>
  make_publisher(const core::CollisionBodyId &endpoint_body_id) {
    auto world = std::make_shared<core::CollisionWorldSnapshot>();
    core::CollisionWorldBody endpoint_body{};
    endpoint_body.body_id = endpoint_body_id;
    core::CollisionWorldBody target_body{};
    target_body.body_id = kTarget;
    core::CollisionWorldBody robot_body{};
    robot_body.body_id = kRobotBody;
    world->bodies = {std::move(endpoint_body), std::move(target_body)};
    if (endpoint_body_id != kRobotBody) world->bodies.push_back(std::move(robot_body));
    std::sort(world->bodies.begin(), world->bodies.end(),
              [](const auto &a, const auto &b) { return a.body_id < b.body_id; });
    auto bundle = std::make_shared<core::RuntimeCollisionQueryBundle>();
    bundle->world = world;
    bundle->lookup_order_valid = true;
    return std::make_shared<Publisher>(std::move(bundle));
  }

  GeometricContactInputBinding make_binding(
      core::IoSourceIncarnation incarnation,
      const core::CollisionBodyId &endpoint_body_id,
      std::shared_ptr<Publisher> selected_publisher,
      std::shared_ptr<Engine> selected_engine,
      std::vector<std::shared_ptr<const core::ObservedRobotStatePublication>>
          observed,
      core::Duration age = 25) {
    return {kSource,
            std::move(incarnation),
            {descriptor(age)},
            [endpoint_body_id] {
              return core::Result<std::vector<core::CollisionBodyId>>::Ok({endpoint_body_id});
            },
            core::SceneResourceId("fixture.target"),
            std::move(observed),
            std::move(selected_publisher),
            std::move(selected_engine),
            std::shared_ptr<core::IScheduler>(&scheduler,
                                              [](core::IScheduler *) {}),
            10,
            5,
            [this](const core::CollisionWorldSnapshot &,
                   const core::CollisionBodyId &body) -> std::optional<bool> {
              const auto found = body_classes.find(body);
              return found == body_classes.end() ? std::optional<bool>(false)
                                                 : found->second;
            }};
  }

  void Successor(
      const core::IoSourceIncarnation &incarnation,
      std::shared_ptr<core::IoSignalSubsystem> &next_signals,
      std::shared_ptr<core::IoSignalIngress> &next_ingress) {
    auto created = core::IoSignalSubsystem::Create(
        {core::RequestOwnerEpoch("host"), 2, {descriptor()}, {}},
        {1, 1, 8, 4, 128, 1024}, clock);
    ASSERT_TRUE(created.ok()) << created.error().message;
    next_signals = std::shared_ptr<core::IoSignalSubsystem>(
        std::move(created.value()));
    ASSERT_TRUE(next_signals->InstallSource(kSource, incarnation, std::nullopt)
                    .ok());
    auto queued = core::IoSignalIngress::Create(next_signals);
    ASSERT_TRUE(queued.ok());
    next_ingress =
        std::shared_ptr<core::IoSignalIngress>(std::move(queued.value()));
  }

  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::vector<core::CollisionBodyId> selected_bodies{kEndpoint};
  std::optional<core::ErrorDTO> selection_error;
  std::map<core::CollisionBodyId, std::optional<bool>> body_classes;
  std::shared_ptr<core::ObservedRobotStatePublication> motion, gripper;
  std::shared_ptr<Publisher> publisher_;
  std::shared_ptr<Engine> engine;
  std::shared_ptr<core::IoSignalSubsystem> signals;
  std::shared_ptr<core::IoSignalIngress> ingress;
  std::unique_ptr<GeometricContactInputProvider> provider;
};

TEST_F(GeometricContactInputProviderTest, ImmediateContactUsesBothReportedStatesAndAuthoredAge) {
  Build();
  const auto baseline = provider->Resume(2);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.value, true);
  EXPECT_EQ(observed.latest_sample->publication.source_sequence,
            baseline.value().source_sequence);
  EXPECT_EQ(engine->prepared_names,
            (std::vector<std::string>{"arm_joint", "finger_joint"}));
  EXPECT_EQ(engine->positions, (std::vector<double>{0.4, 0.2}));
  EXPECT_EQ(engine->state_time, 90);
  clock.set(126);
  EXPECT_EQ(signals->ReadSignal(kSignal).value().availability,
            core::IoSignalAvailability::kStale);
}

// A new Active TCP changes the endpoint body set on the next sample while
// the provider remains enabled. The engine keeps reporting the same gripper
// collision, so only the selected body set changes the observed level.
TEST_F(GeometricContactInputProviderTest, ActiveTcpChangeSelectsCurrentBodiesEverySample) {
  Build();
  ASSERT_TRUE(provider->Resume(2).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.latest_sample->publication.value, std::optional<bool>(true));

  selected_bodies = {kRobotBody};
  clock.set(110);
  scheduler.advance_to(110, core::SchedulerPriority::Monitoring);
  ASSERT_TRUE(ingress->Drain(8).ok());
  observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.value, std::optional<bool>(false));
  EXPECT_EQ(engine->queries, 2U);

  selected_bodies = {kEndpoint};
  clock.set(120);
  scheduler.advance_to(120, core::SchedulerPriority::Monitoring);
  ASSERT_TRUE(ingress->Drain(8).ok());
  observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.latest_sample->publication.value, std::optional<bool>(true));
  EXPECT_EQ(engine->queries, 3U);
}

TEST_F(GeometricContactInputProviderTest, StaticAndUnknownBodiesAreUnavailableThenMovingResumes) {
  Build();
  body_classes[kEndpoint] = true;
  ASSERT_TRUE(provider->Resume(2).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_FALSE(observed.latest_sample->publication.value.has_value());
  ASSERT_TRUE(observed.latest_sample->publication.error);
  EXPECT_EQ(observed.latest_sample->publication.error->code, "io_source_unavailable");
  EXPECT_EQ(observed.latest_sample->publication.error->details.at("reason"), "contact_bodies_static");
  EXPECT_EQ(engine->queries, 0U);

  body_classes[kEndpoint] = std::nullopt;
  clock.set(110);
  scheduler.advance_to(110, core::SchedulerPriority::Monitoring);
  ASSERT_TRUE(ingress->Drain(8).ok());
  observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  ASSERT_TRUE(observed.latest_sample->publication.error);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_EQ(observed.latest_sample->publication.error->details.at("reason"), "collision_geometry_unresolved");
  EXPECT_EQ(engine->queries, 0U);

  body_classes[kEndpoint] = false;
  clock.set(120);
  scheduler.advance_to(120, core::SchedulerPriority::Monitoring);
  ASSERT_TRUE(ingress->Drain(8).ok());
  observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.value, std::optional<bool>(true));
  EXPECT_EQ(engine->queries, 1U);

  selected_bodies = {kEndpoint, kRobotBody};
  body_classes[kRobotBody] = true;
  clock.set(130);
  scheduler.advance_to(130, core::SchedulerPriority::Monitoring);
  ASSERT_TRUE(ingress->Drain(8).ok());
  observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kUnavailable);
  ASSERT_TRUE(observed.latest_sample->publication.error);
  EXPECT_EQ(observed.latest_sample->publication.error->details.at("reason"), "contact_bodies_static");
  EXPECT_EQ(engine->queries, 1U);
}

TEST_F(GeometricContactInputProviderTest, UnavailableSelectionNeverPublishesFalse) {
  Build();
  selection_error = core::ErrorDTO{"execution_rejected", "no Active TCP", {{"reason", "active_tcp_unavailable"}}};
  ASSERT_TRUE(provider->Resume(2).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  auto initial = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(initial.latest_sample);
  EXPECT_EQ(initial.availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_FALSE(initial.latest_sample->publication.value.has_value());
  ASSERT_TRUE(initial.latest_sample->publication.error);
  EXPECT_EQ(initial.latest_sample->publication.error->code, "io_source_unavailable");
  EXPECT_EQ(initial.latest_sample->publication.error->details.at("reason"), "active_tcp_unavailable");
  ASSERT_TRUE(provider->Pause(3));
  ASSERT_TRUE(ingress->Drain(8).ok());
  selection_error = core::ErrorDTO{"execution_rejected", "tool not mounted", {{"reason", "tcp_tool_not_mounted"}}};
  ASSERT_TRUE(provider->Resume(4).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_FALSE(observed.latest_sample->publication.value.has_value());
  ASSERT_TRUE(observed.latest_sample->publication.error);
  EXPECT_EQ(observed.latest_sample->publication.error->details.at("reason"), "tcp_tool_not_mounted");
  ASSERT_TRUE(provider->Pause(5));
  ASSERT_TRUE(ingress->Drain(8).ok());
  selection_error.reset();
  selected_bodies.clear();
  ASSERT_TRUE(provider->Resume(6).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_FALSE(observed.latest_sample->publication.value.has_value());
  ASSERT_TRUE(observed.latest_sample->publication.error);
  EXPECT_EQ(observed.latest_sample->publication.error->details.at("reason"),
            "active_tcp_contact_bodies_unavailable");
  EXPECT_EQ(engine->queries, 0U);
}

TEST_F(GeometricContactInputProviderTest, StateReceivedAfterTheQueryClockIsCurrent) {
  Build();
  // Received after the provider's clock read (100): current
  // (received_sample_age.hpp), not missing or stale.
  const std::vector<std::string> finger{"finger_joint"};
  const std::vector<double> finger_position{0.3};
  const std::vector<double> velocity{0.0};
  ASSERT_TRUE(gripper->publish(core::ObservedRobotStateSourceKind::kJointState,
                               finger, finger_position, velocity, 105, 105)
                  .published);
  const auto baseline = provider->Resume(2);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.value, true);
  EXPECT_EQ(engine->positions, (std::vector<double>{0.4, 0.3}));
  EXPECT_EQ(engine->state_time, 90);
}

TEST_F(GeometricContactInputProviderTest, MissingStateIsExplicitUnavailableBaseline) {
  Build(false);
  const auto baseline = provider->Resume(2);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_EQ(observed.latest_sample->publication.source_sequence,
            baseline.value().source_sequence);
  EXPECT_EQ(engine->queries, 0U);
}

TEST_F(GeometricContactInputProviderTest,
       ClearQueryPublishesFalseAndInconclusiveQueryPublishesUnavailable) {
  Build();
  engine->status = core::RuntimeCollisionQueryStatus::kClear;
  auto baseline = provider->Resume(2);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(ingress->Drain(8).ok());
  auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.value, false);

  ASSERT_TRUE(provider->Pause(3));
  ASSERT_TRUE(ingress->Drain(8).ok());
  engine->status = core::RuntimeCollisionQueryStatus::kInconclusive;
  engine->failure = core::RuntimeCollisionQueryFailureReason::kTimeout;
  baseline = provider->Resume(4);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(ingress->Drain(8).ok());
  observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kUnavailable);
  EXPECT_EQ(observed.latest_sample->publication.source_sequence,
            baseline.value().source_sequence);
}

TEST_F(GeometricContactInputProviderTest,
       StaleReportedStateAndUnresolvedGeometryRemainUnavailable) {
  Build();
  clock.set(151);
  auto baseline = provider->Resume(2);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_EQ(signals->ReadSignal(kSignal).value().availability,
            core::IoSignalAvailability::kUnavailable);
  EXPECT_EQ(engine->queries, 0U);

  ASSERT_TRUE(provider->Pause(3));
  ASSERT_TRUE(ingress->Drain(8).ok());
  const std::vector<std::string> arm{"arm_joint"};
  const std::vector<double> arm_position{0.5};
  const std::vector<double> velocity{0.0};
  ASSERT_TRUE(motion->publish(core::ObservedRobotStateSourceKind::kJointState,
                              arm, arm_position, velocity, 151, 151)
                  .published);
  const std::vector<std::string> finger{"finger_joint"};
  const std::vector<double> finger_position{0.3};
  ASSERT_TRUE(gripper->publish(
                          core::ObservedRobotStateSourceKind::kJointState,
                          finger, finger_position, velocity, 151, 151)
                  .published);
  engine->snapshot_ready = false;
  baseline = provider->Resume(4);
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_EQ(signals->ReadSignal(kSignal).value().availability,
            core::IoSignalAvailability::kUnavailable);
  EXPECT_EQ(engine->queries, 0U);
}

TEST_F(GeometricContactInputProviderTest, ResumeRefusesAnUnenqueuedBaseline) {
  Build(true, 1);
  ASSERT_TRUE(ingress->TryEnqueue(
      {kSource, kIncarnation, 1, clock.now(),
       {{kSignal, core::IoSampleValidity::kUnavailable, std::nullopt,
         core::ErrorDTO{"io_source_unavailable", "old", {}}}}}));
  const auto baseline = provider->Resume(2);
  ASSERT_FALSE(baseline.ok());
  EXPECT_EQ(baseline.error().details.at("reason"),
            "initial_acquisition_not_enqueued");
  EXPECT_EQ(engine->queries, 1U);
  clock.set(1'000);
  scheduler.advance_to(clock.now(), core::SchedulerPriority::Monitoring);
  EXPECT_EQ(engine->queries, 1U);
}

TEST_F(GeometricContactInputProviderTest, LateQuerySkipsCatchupDeadlines) {
  Build();
  engine->clock = &clock;
  engine->duration = 25;
  ASSERT_TRUE(provider->Resume(2).ok());
  EXPECT_EQ(clock.now(), 125);
  scheduler.advance_to(129, core::SchedulerPriority::Monitoring);
  EXPECT_EQ(engine->queries, 1U);
  clock.set(130);
  scheduler.advance_to(130, core::SchedulerPriority::Monitoring);
  EXPECT_EQ(engine->queries, 2U);
  EXPECT_EQ(scheduler.pending_count(core::SchedulerPriority::Monitoring), 1U);
}

TEST_F(GeometricContactInputProviderTest,
       AbortDropsInflightReceiptAndStaleAbortCannotCloseNewerResume) {
  Build();
  {
    std::lock_guard lock(engine->block_mutex);
    engine->block = true;
  }
  auto old_resume = std::async(std::launch::async, [&] {
    return provider->Resume(2);
  });
  {
    std::unique_lock lock(engine->block_mutex);
    ASSERT_TRUE(engine->block_cv.wait_for(
        lock, std::chrono::seconds(2), [&] { return engine->entered; }));
  }
  ASSERT_TRUE(provider->AbortResume(2));
  {
    std::lock_guard lock(engine->block_mutex);
    engine->block = false;
  }
  engine->block_cv.notify_all();
  EXPECT_FALSE(old_resume.get().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_EQ(signals->ReadSignal(kSignal).value().availability,
            core::IoSignalAvailability::kUnavailable);

  const auto current = provider->Resume(3);
  ASSERT_TRUE(current.ok()) << current.error().message;
  EXPECT_FALSE(provider->AbortResume(2));
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto observed = signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.source_sequence,
            current.value().source_sequence);
}

TEST_F(GeometricContactInputProviderTest,
       RebindAdoptsChangedResolvedEndpointAndFencesOldInflightReceipt) {
  Build();
  {
    std::lock_guard lock(engine->block_mutex);
    engine->block = true;
  }
  auto old_resume = std::async(std::launch::async, [&] {
    return provider->Resume(2);
  });
  {
    std::unique_lock lock(engine->block_mutex);
    ASSERT_TRUE(engine->block_cv.wait_for(
        lock, std::chrono::seconds(2), [&] { return engine->entered; }));
  }

  const core::IoSourceIncarnation next_incarnation("activation.2");
  std::shared_ptr<core::IoSignalSubsystem> next_signals;
  std::shared_ptr<core::IoSignalIngress> next_ingress;
  Successor(next_incarnation, next_signals, next_ingress);
  const core::CollisionBodyId projected_endpoint(
      "component/robot.arm/model/arm/body/projected_tool/collision/0");
  auto next_publisher = make_publisher(projected_endpoint);
  auto next_engine = std::make_shared<Engine>(projected_endpoint);
  auto next_binding = make_binding(next_incarnation, projected_endpoint,
                                   next_publisher, next_engine,
                                   {motion, gripper});
  const auto rebound = provider->RebindIngress(
      next_ingress, std::move(next_binding), 3);
  ASSERT_TRUE(rebound.ok()) << rebound.error().message;

  {
    std::lock_guard lock(engine->block_mutex);
    engine->block = false;
  }
  engine->block_cv.notify_all();
  EXPECT_FALSE(old_resume.get().ok());
  ASSERT_TRUE(next_ingress->Drain(8).ok());
  const auto observed = next_signals->ReadSignal(kSignal).value();
  ASSERT_TRUE(observed.latest_sample);
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.incarnation,
            next_incarnation);
  EXPECT_EQ(observed.latest_sample->publication.source_sequence, 1U);
  EXPECT_EQ(next_engine->queries, 1U);
}

} // namespace
} // namespace manyforge::roboplan_backend
