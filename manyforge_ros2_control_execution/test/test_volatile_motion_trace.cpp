#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <fstream>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_ros2_control_execution/volatile_motion_trace.hpp>

namespace manyforge::ros2_control_execution {
namespace {
using namespace core;
using V = content_identity::CanonicalValue;
ExecutionGoalKey Key(const char *name, std::uint64_t generation = 1U) {
  return {ControllerGoalId(name), ControllerGoalGeneration(generation)};
}
VolatileMotionTrace::Goal Goal(double position = 1.0) {
  VolatileMotionTrace::Goal goal;
  goal.trajectory.joint_names = {"j1"};
  trajectory_msgs::msg::JointTrajectoryPoint point;
  point.time_from_start.sec = -1;
  point.time_from_start.nanosec = 500000000U;
  point.positions = {position};
  point.velocities = {0.25};
  point.accelerations = {-0.5};
  goal.trajectory.points.push_back(std::move(point));
  return goal;
}
const V &Member(const V &value, const char *name) {
  const auto *member = value.find_object_member(name);
  if (!member)
    throw std::logic_error(name);
  return *member;
}
class MotionTraceFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    char name[] = "/tmp/manyforge-trace-file-XXXXXX";
    const auto *created = ::mkdtemp(name);
    ASSERT_NE(created, nullptr);
    directory = created;
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  std::string Read(const V &metadata) {
    std::ifstream input(directory / Member(metadata, "file").as_string());
    return std::string(std::istreambuf_iterator<char>(input), {});
  }
  void Seed(std::size_t index, std::size_t bytes) {
    auto digits = std::to_string(index);
    const auto name = "trace-" + std::string(64U - digits.size(), '0') + digits + ".json";
    const auto path = directory / name;
    std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
    ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
    const timespec times[2]{{static_cast<time_t>(index + 1U), 0},
                           {static_cast<time_t>(index + 1U), 0}};
    ASSERT_EQ(::utimensat(AT_FDCWD, path.c_str(), times, 0), 0);
  }
  std::filesystem::path directory;
};
TEST_F(MotionTraceFileTest, LargeOrdinaryAndReplacementGoalsAreReplayableAcrossOwnerRestart) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto old = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U), Key("old"), {}, 10, garbage);
  for (const bool replacement : {false, true}) {
    auto goal = Goal(replacement ? -0.125 : 0.125);
    auto point = goal.trajectory.points.front();
    point.velocities = {-0.0};
    goal.trajectory.header.stamp.sec = replacement ? 100 : 0;
    goal.trajectory.points.clear();
    for (std::size_t i = 0; i < 8000U; ++i) {
      point.time_from_start.sec = static_cast<int>(i / 1000U);
      point.time_from_start.nanosec = static_cast<unsigned>(i % 1000U) * 1000000U;
      goal.trajectory.points.push_back(point);
    }
    auto token = replacement ? trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U),
        Key("new"), old, 20, garbage) : old;
    std::optional<ExecutionReplacementStartDTO> start = replacement
        ? std::optional(ExecutionReplacementStartDTO{ExecutionBindingEpoch(7U), 100000000000LL, {}})
        : std::nullopt;
    if (start) {
      start->experiment = ExecutionReplacementExperimentDTO{};
      start->experiment->release_hold = ExecutionReleaseHold::kHeld;
      start->experiment->factor = 0.5;
    }
    trace.Attach(token, std::move(goal), 30, ExecutionPortDispatch::kDispatched,
                 garbage, replacement, start);
  }
  trace.PublishedScale("/arm/fjt", ExecutionBindingEpoch(7U), 25, 0.5);
  const auto result = trace.SnapshotFile(RequestOwnerEpoch("owner.old"),
      VolatileMotionTrace::kMaximumTraceBytes, directory);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &metadata = result.value();
  const auto bytes = Read(metadata);
  EXPECT_GT(bytes.size(), 212992U);
  EXPECT_LT(content_identity::canonical_json(metadata).size(), 1024U);
  EXPECT_EQ(content_identity::sha256_digest(bytes), Member(metadata, "sha256").as_string());
  const auto snapshot = content_identity::parse_canonical_json(bytes);
  const auto &goals = Member(snapshot, "goals").as_array();
  ASSERT_EQ(goals.size(), 2U);
  EXPECT_EQ(Member(goals[0], "controller_goal_id").as_string(), "new");
  EXPECT_EQ(Member(goals[1], "controller_goal_id").as_string(), "old");
  EXPECT_EQ(Member(Member(goals[0], "trajectory"), "points").as_array().size(), 8000U);
  EXPECT_EQ(Member(Member(goals[1], "trajectory"), "points").as_array().size(), 8000U);
  EXPECT_EQ(Member(Member(goals[0], "replacement_policy"), "schedule").as_string(), "controller_stamp");
  const auto &policy = Member(Member(goals[0], "replacement_policy"), "experiment");
  EXPECT_EQ(Member(policy, "release_hold").as_string(), "held");
  EXPECT_EQ(Member(policy, "factor").as_number(), 0.5);
  EXPECT_EQ(Member(snapshot, "scale_events").as_array().size(), 1U);
  VolatileMotionTrace restarted;
  const auto later = restarted.SnapshotFile(RequestOwnerEpoch("owner.new"), 4096U, directory);
  ASSERT_TRUE(later.ok()) << later.error().message;
  EXPECT_EQ(Read(metadata), bytes);
  EXPECT_EQ(Member(content_identity::parse_canonical_json(Read(later.value())),
                   "request_owner_epoch").as_string(), "owner.new");
}
TEST_F(MotionTraceFileTest, CountsTemporariesAndEvictsOldestAcrossAllOwners) {
  for (std::size_t i = 0; i < 21U; ++i) Seed(i, 3U * 1024U * 1024U);
  const auto stale = directory / ".snapshot.tmp";
  std::ofstream(stale) << "interrupted";
  ASSERT_EQ(::chmod(stale.c_str(), 0600), 0);
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  for (std::size_t i = 0; i < 16U; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
        {ControllerGoalId("goal." + std::to_string(i)), ControllerGoalGeneration(1U)}, {}, 0, garbage);
    auto goal = Goal();
    goal.trajectory.points.resize(1000U, goal.trajectory.points.front());
    trace.Attach(token, std::move(goal), 1, ExecutionPortDispatch::kDispatched, garbage);
  }
  const auto result = trace.SnapshotFile(RequestOwnerEpoch("owner"),
      VolatileMotionTrace::kMaximumTraceBytes, directory);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(std::filesystem::exists(stale));
  std::uint64_t total = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    struct stat status {};
    ASSERT_EQ(::lstat(entry.path().c_str(), &status), 0);
    total += std::max(static_cast<std::uint64_t>(status.st_size),
                     static_cast<std::uint64_t>(status.st_blocks) * 512U);
  }
  EXPECT_LE(total, 64U * 1024U * 1024U);
  EXPECT_FALSE(std::filesystem::exists(directory / ("trace-" + std::string(64U, '0') + ".json")));
  // No per-owner subdirectory escapes the global inventory.
  EXPECT_EQ(Member(result.value(), "status").as_string(), "partial");
}
TEST_F(MotionTraceFileTest, RefusesUnsafeStoresAndConcurrentReaderWithoutLeavingPartialFile) {
  VolatileMotionTrace trace;
  const auto owner = RequestOwnerEpoch("owner");
  const auto link = directory / ("trace-" + std::string(64U, 'a') + ".json");
  ASSERT_EQ(::symlink("/etc/passwd", link.c_str()), 0);
  EXPECT_FALSE(trace.SnapshotFile(owner, 4096U, directory).ok());
  std::filesystem::remove(link);
  const auto fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::flock(fd, LOCK_SH | LOCK_NB), 0);
  const auto busy = trace.SnapshotFile(owner, 4096U, directory);
  EXPECT_FALSE(busy.ok());
  EXPECT_EQ(busy.error().code, "motion_trace_store_busy");
  ::close(fd);
  EXPECT_TRUE(std::filesystem::is_empty(directory));
  EXPECT_FALSE(trace.SnapshotFile(owner, 4096U, "relative-directory").ok());
  EXPECT_FALSE(trace.SnapshotFile(owner, 0U, directory).ok());
}
TEST_F(MotionTraceFileTest, AggregateCapIncludesStaleTemporaryBeforeAnyCleanup) {
  for (std::size_t i = 0; i < 21U; ++i) Seed(i, 3U * 1024U * 1024U);
  const auto stale = directory / ".snapshot.tmp";
  std::ofstream(stale) << std::string(2U * 1024U * 1024U, 'x');
  ASSERT_EQ(::chmod(stale.c_str(), 0600), 0);
  VolatileMotionTrace trace;
  const auto result = trace.SnapshotFile(RequestOwnerEpoch("owner"), 4096U, directory);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "motion_trace_store_full");
  EXPECT_TRUE(std::filesystem::exists(stale));
}
TEST(VolatileMotionTraceTest, NegativeZeroGoalExportsWithoutChangingRetainedData) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  const auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                                 Key("signed.zero"), {}, 1, garbage);
  auto goal = Goal(-0.0);
  auto &point = goal.trajectory.points.front();
  point.velocities = {-0.0, -0.25};
  point.accelerations = {-0.0, 0.5};
  point.effort = {-0.0, 2.5};
  trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
               garbage);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"), 4000U);
  ASSERT_TRUE(result.ok()) << result.error().code << ": " << result.error().message;
  const auto &exported = Member(Member(Member(result.value(), "goals").as_array().front(),
                                       "trajectory"), "points").as_array().front();
  for (const auto *field : {"p", "v", "a", "effort"}) {
    const auto value = Member(exported, field).as_array().front().as_number();
    EXPECT_EQ(value, 0.0);
    EXPECT_FALSE(std::signbit(value));
  }
  EXPECT_EQ(Member(exported, "v").as_array()[1].as_number(), -0.25);
  EXPECT_EQ(Member(exported, "a").as_array()[1].as_number(), 0.5);
  EXPECT_EQ(Member(exported, "effort").as_array()[1].as_number(), 2.5);
  EXPECT_EQ(Member(exported, "time_sec").as_number(), -1.0);
  EXPECT_LE(content_identity::canonical_json(result.value()).size(), 4000U);
  EXPECT_FALSE(trace.Snapshot(RequestOwnerEpoch("owner"), 1U).ok());

  // Eviction exposes the retained payload: encoding must not rewrite it.
  for (std::size_t i = 0; i < VolatileMotionTrace::kGoalLimit; ++i)
    trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                {ControllerGoalId("next." + std::to_string(i)),
                 ControllerGoalGeneration(1U)}, {}, 3, garbage);
  ASSERT_EQ(garbage.size(), 1U);
  const auto &retained = garbage.front()->trajectory.points.front();
  for (const auto *values : {&retained.positions, &retained.velocities,
                             &retained.accelerations, &retained.effort})
    EXPECT_TRUE(std::signbit(values->front()));
  EXPECT_THROW(V(-0.0), content_identity::CanonicalValueError);
}
TEST(VolatileMotionTraceTest, NegativeZeroSamplesExportAtTheDiagnosticBoundary) {
  for (const auto kind : {0, 1, 2}) {
    SCOPED_TRACE(kind);
    VolatileMotionTrace trace;
    ExecutionControllerSampleDTO sample;
    sample.binding_epoch = ExecutionBindingEpoch(1U);
    sample.cursor_s = kind == 1 ? -0.0 : 1.25;
    sample.applied_factor = kind == 2 ? -0.0 : 0.5;
    sample.reference_positions_rad = {kind == 0 ? -0.0 : 0.0, 1.25};
    sample.reference_velocities_rad_s = sample.reference_positions_rad;
    sample.reference_accelerations_rad_s2 = sample.reference_positions_rad;
    sample.command_positions_rad = sample.reference_positions_rad;
    sample.command_velocities_rad_s = sample.reference_positions_rad;
    sample.measured_positions_rad = sample.reference_positions_rad;
    sample.measured_velocities_rad_s = sample.reference_positions_rad;
    trace.Sample("/arm/fjt", sample);
    const auto result = trace.Snapshot(RequestOwnerEpoch("owner"), 4000U);
    ASSERT_TRUE(result.ok()) << result.error().code << ": " << result.error().message;
    const auto &row = Member(result.value(), "controller_samples").as_array().front();
    for (const auto *field : {"reference_p", "reference_v", "reference_a",
                              "commanded_p", "commanded_v", "measured_p", "measured_v"}) {
      const auto &values = Member(row, field).as_array();
      EXPECT_FALSE(std::signbit(values.front().as_number()));
      EXPECT_EQ(values[1].as_number(), 1.25);
    }
    EXPECT_EQ(Member(row, "cursor_s").as_number(), kind == 1 ? 0.0 : 1.25);
    EXPECT_FALSE(std::signbit(Member(row, "cursor_s").as_number()));
    EXPECT_EQ(Member(row, "applied_factor").as_number(), kind == 2 ? 0.0 : 0.5);
    EXPECT_FALSE(std::signbit(Member(row, "applied_factor").as_number()));
    EXPECT_EQ(std::signbit(sample.reference_positions_rad.front()), kind == 0);
    EXPECT_EQ(std::signbit(sample.cursor_s), kind == 1);
    EXPECT_EQ(std::signbit(*sample.applied_factor), kind == 2);
    EXPECT_LE(content_identity::canonical_json(result.value()).size(), 4000U);
  }
}
TEST(VolatileMotionTraceTest, NegativeZeroPublishedFactorExportsAsCanonicalZero) {
  VolatileMotionTrace trace;
  trace.PublishedScale("/arm/fjt", ExecutionBindingEpoch(1U), 2, -0.0);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"), 4000U);
  ASSERT_TRUE(result.ok()) << result.error().code << ": " << result.error().message;
  const auto &row = Member(result.value(), "scale_events").as_array().front();
  EXPECT_EQ(Member(row, "kind").as_string(), "published");
  EXPECT_EQ(Member(row, "factor").as_number(), 0.0);
  EXPECT_FALSE(std::signbit(Member(row, "factor").as_number()));
}
TEST(VolatileMotionTraceTest,
     EarlyFeedbackSurvivesPayloadAttachmentAndSignedPointsRemainWhole) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  const auto key = Key("goal.one");
  auto token =
      trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U), key, {}, 10, garbage);
  ASSERT_TRUE(token);
  ExecutionGoalObservationDTO facts;
  facts.key = key;
  facts.binding_epoch = ExecutionBindingEpoch(7U);
  facts.mechanism_goal_id = MechanismGoalId("uuid.actual");
  facts.accepted_stamp_ns = 11;
  facts.latest_feedback = ExecutionGoalFeedbackDTO{};
  facts.latest_feedback->local_receipt_stamp_ns = 12;
  trace.Observe(token, facts); // Callback before the SDK send returns.
  auto goal = Goal();
  trace.Attach(token, std::move(goal), 13, ExecutionPortDispatch::kDispatched,
               garbage);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner.actual"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &row = Member(result.value(), "goals").as_array().front();
  EXPECT_EQ(Member(row, "mechanism_goal_id").as_string(), "uuid.actual");
  EXPECT_EQ(Member(row, "accepted_at_ns").as_number(), 11.0);
  EXPECT_EQ(Member(row, "first_own_feedback_at_ns").as_number(), 12.0);
  EXPECT_EQ(Member(row, "installation").as_string(), "unknown");
  const auto &point =
      Member(Member(row, "trajectory"), "points").as_array().front();
  EXPECT_EQ(Member(point, "time_sec").as_number(), -1.0);
  EXPECT_EQ(Member(point, "time_nanosec").as_number(), 500000000.0);
  EXPECT_EQ(Member(point, "p").as_array().front().as_number(), 1.0);
  const auto encoded = content_identity::canonical_json(result.value());
  EXPECT_EQ(
      Member(Member(result.value(), "coverage"), "exported_bytes").as_number(),
      static_cast<double>(encoded.size()));
  EXPECT_TRUE(goal.trajectory.points.empty());
}
TEST(VolatileMotionTraceTest,
     OnlyExactStopGenerationCanInheritOperationContext) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto parent =
      std::make_shared<VolatileMotionTrace::Token>(VolatileMotionTrace::Token{
          "/arm/fjt", ExecutionBindingEpoch(7U), Key("goal.one"),
          VolatileMotionTrace::Context{"op.actual", "participant.actual",
                                       "run.actual", "binding.actual",
                                       "robot.actual", 3U}});
  auto stop = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U),
                          Key("goal.one", 2U), parent, 10, garbage);
  auto newcomer = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U),
                              Key("goal.new"), parent, 11, garbage);
  auto epoch = trace.Begin("/arm/fjt", ExecutionBindingEpoch(8U),
                           Key("goal.one", 2U), parent, 12, garbage);
  ASSERT_TRUE(stop);
  ASSERT_TRUE(newcomer);
  ASSERT_TRUE(epoch);
  ASSERT_TRUE(stop->context);
  EXPECT_EQ(stop->context->operation_id, "op.actual");
  EXPECT_FALSE(newcomer->context);
  EXPECT_FALSE(epoch->context);
}
TEST(VolatileMotionTraceTest,
     UnsignedIdentityBoundariesRemainExactAndDistinct) {
  constexpr std::uint64_t safe = 9007199254740991ULL;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto parent =
      std::make_shared<VolatileMotionTrace::Token>(VolatileMotionTrace::Token{
          "/arm/fjt", ExecutionBindingEpoch(safe + 2U), Key("goal.exact", safe),
          VolatileMotionTrace::Context{"op.actual", "participant.actual",
                                       "run.actual", "binding.actual",
                                       "robot.actual", maximum}});
  auto first = trace.Begin("/arm/fjt", ExecutionBindingEpoch(safe + 2U),
                           Key("goal.exact", safe + 1U), parent, 10, garbage);
  auto second = trace.Begin("/arm/fjt", ExecutionBindingEpoch(safe + 2U),
                            Key("goal.exact", safe + 2U), first, 11, garbage);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  trace.Attach(first, Goal(), 12, ExecutionPortDispatch::kDispatched, garbage);
  trace.Attach(second, Goal(), 13, ExecutionPortDispatch::kDispatched, garbage);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  const auto &rows = Member(result.value(), "goals").as_array();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Member(rows[0], "controller_goal_generation").as_string(),
            "9007199254740993");
  EXPECT_EQ(Member(rows[1], "controller_goal_generation").as_string(),
            "9007199254740992");
  EXPECT_EQ(Member(rows[0], "binding_epoch").as_string(), "9007199254740993");
  EXPECT_EQ(Member(rows[0], "operation_revision").as_string(),
            "18446744073709551615");
  EXPECT_EQ(Member(Member(rows[0], "predecessor"), "controller_goal_generation")
                .as_string(),
            "9007199254740992");
  EXPECT_EQ(Member(Member(rows[1], "predecessor"), "controller_goal_generation")
                .as_number(),
            static_cast<double>(safe));
}
TEST(VolatileMotionTraceTest,
     EvictionDetachesPayloadAndCallbackTokenDoesNotPinIt) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto first = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U), Key("first"),
                           {}, 1, garbage);
  trace.Attach(first, Goal(), 2, ExecutionPortDispatch::kDispatched, garbage);
  for (std::size_t i = 0; i < VolatileMotionTrace::kGoalLimit; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             {ControllerGoalId("goal." + std::to_string(i)),
                              ControllerGoalGeneration(1U)},
                             {}, 3, garbage);
    trace.Attach(token, Goal(), 4, ExecutionPortDispatch::kDispatched, garbage);
  }
  ASSERT_EQ(garbage.size(), 1U);
  std::weak_ptr<const VolatileMotionTrace::Goal> retired = garbage.front();
  garbage.clear();
  EXPECT_TRUE(retired.expired());
  EXPECT_TRUE(first);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(Member(result.value(), "goals").as_array().size(), 16U);
  EXPECT_EQ(
      Member(Member(result.value(), "coverage"), "evicted_goals").as_number(),
      1.0);
}
TEST(VolatileMotionTraceTest,
     TinyAllowanceRefusesAndLargerCutoffOmitsWholeGoal) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U), Key("large"),
                           {}, 1, garbage);
  auto goal = Goal();
  goal.trajectory.points.resize(200U, goal.trajectory.points.front());
  trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
               garbage);
  EXPECT_FALSE(trace.Snapshot(RequestOwnerEpoch("owner"), 1U).ok());
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"), 2000U);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(Member(result.value(), "goals").as_array().empty());
  EXPECT_TRUE(
      Member(Member(result.value(), "coverage"), "truncated").as_bool());
  EXPECT_EQ(Member(Member(result.value(), "coverage"), "omitted_goals")
                .as_array()
                .size(),
            1U);
  EXPECT_LE(content_identity::canonical_json(result.value()).size(), 2000U);
}
TEST(VolatileMotionTraceTest,
     LifetimeChargeIsBoundedUntilDetachedPinsAreReleased) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  std::vector<VolatileMotionTrace::TokenPtr> pins;
  bool exhausted = false;
  for (int i = 0; i < 160; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             {ControllerGoalId("pinned." + std::to_string(i)),
                              ControllerGoalGeneration(1U)},
                             {}, 1, garbage);
    if (!token) {
      exhausted = true;
      break;
    }
    trace.Attach(token, Goal(), 2, ExecutionPortDispatch::kDispatched, garbage);
    pins.push_back(std::move(token));
  }
  ASSERT_TRUE(exhausted);
  EXPECT_FALSE(trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                           Key("blocked"), {}, 3, garbage));
  pins.clear();
  garbage.clear();
  auto resumed = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             Key("after_release"), {}, 4, garbage);
  ASSERT_TRUE(resumed);
  trace.Attach(resumed, Goal(), 5, ExecutionPortDispatch::kDispatched, garbage);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_GE(Member(Member(result.value(), "coverage"), "omitted_captures")
                .as_number(),
            2.0);
  EXPECT_EQ(Member(Member(result.value(), "goals").as_array().front(),
                   "controller_goal_id")
                .as_string(),
            "after_release");
}
TEST(VolatileMotionTraceTest,
     ResidentByteBoundEvictsAndOversizedCaptureHasNoPartialPoints) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  for (int i = 0; i < 8; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             {ControllerGoalId("large." + std::to_string(i)),
                              ControllerGoalGeneration(1U)},
                             {}, 1, garbage);
    auto goal = Goal();
    goal.trajectory.points.resize(5000U, goal.trajectory.points.front());
    trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
                 garbage);
    garbage.clear();
  }
  const auto bounded = trace.Snapshot(RequestOwnerEpoch("owner"), 2000U);
  ASSERT_TRUE(bounded.ok());
  EXPECT_GT(
      Member(Member(bounded.value(), "coverage"), "evicted_goals").as_number(),
      0.0);

  VolatileMotionTrace oversize;
  auto token = oversize.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                              Key("oversize"), {}, 1, garbage);
  auto goal = Goal();
  goal.trajectory.points.front().positions.resize(
      VolatileMotionTrace::kGoalBytes / sizeof(double) + 1U);
  oversize.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
                  garbage);
  const auto result = oversize.Snapshot(RequestOwnerEpoch("owner"), 2000U);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(Member(result.value(), "goals").as_array().empty());
  const auto &omissions =
      Member(Member(result.value(), "coverage"), "omitted_goals").as_array();
  ASSERT_EQ(omissions.size(), 1U);
  EXPECT_EQ(Member(omissions.front(), "reason").as_string(),
            "payload_not_retained");
}
TEST(VolatileMotionTraceTest,
     SamplesHaveIndependentBoundsAndScalePublicationIsDistinct) {
  VolatileMotionTrace trace;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.applied_factor = 0.5;
  sample.reference_positions_rad = {1.0};
  sample.command_positions_rad = {1.0};
  sample.measured_positions_rad = {0.99};
  for (std::size_t i = 0; i < VolatileMotionTrace::kSampleLimit + 3U; ++i) {
    sample.local_receipt_stamp_ns = static_cast<TimePoint>(i);
    trace.Sample("/arm/fjt", sample);
  }
  trace.PublishedScale("/arm/fjt", ExecutionBindingEpoch(1U), 2000, 0.25);
  const auto result = trace.Snapshot(RequestOwnerEpoch("owner"),
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_LE(Member(result.value(), "controller_samples").as_array().size(),
            VolatileMotionTrace::kSampleLimit);
  EXPECT_GE(
      Member(Member(result.value(), "coverage"), "dropped_samples").as_number(),
      3.0);
  const auto &events = Member(result.value(), "scale_events").as_array();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(Member(events.front(), "kind").as_string(), "published");
  EXPECT_EQ(Member(events.back(), "kind").as_string(), "applied_observed");
}
} // namespace
} // namespace manyforge::ros2_control_execution
