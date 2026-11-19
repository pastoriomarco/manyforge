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
#include <cstdlib>
#include <new>

// Counts this thread's allocations and frees while enabled: the sample
// capture path may do neither.
namespace {
thread_local bool count_heap = false;
thread_local std::size_t heap_calls = 0U;
void *CountedAllocate(std::size_t size) {
  if (count_heap) ++heap_calls;
  if (void *memory = std::malloc(size == 0U ? 1U : size)) return memory;
  throw std::bad_alloc();
}
[[gnu::noinline]] void CountedFree(void *memory) noexcept {
  if (count_heap && memory) ++heap_calls;
  std::free(memory);
}
template <typename Call> std::size_t HeapCallsDuring(Call &&call) {
  heap_calls = 0U;
  count_heap = true;
  call();
  count_heap = false;
  return heap_calls;
}
} // namespace
void *operator new(std::size_t size) { return CountedAllocate(size); }
void *operator new[](std::size_t size) { return CountedAllocate(size); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  if (count_heap) ++heap_calls;
  return std::malloc(size == 0U ? 1U : size);
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  if (count_heap) ++heap_calls;
  return std::malloc(size == 0U ? 1U : size);
}
void operator delete(void *memory) noexcept { CountedFree(memory); }
void operator delete[](void *memory) noexcept { CountedFree(memory); }
void operator delete(void *memory, std::size_t) noexcept { CountedFree(memory); }
void operator delete[](void *memory, std::size_t) noexcept { CountedFree(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept { CountedFree(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { CountedFree(memory); }

namespace manyforge::ros2_control_execution {
namespace {
using namespace core;
using V = content_identity::CanonicalValue;
// One goal of participant `ordinal` of operation 1.
ControllerGoalId Key(std::uint32_t ordinal, std::uint32_t generation = 1U) {
  return {OperationNumber(1U), ordinal, generation};
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
double Ordinal(const V &row) {
  return Member(Member(row, "controller_goal_id"), "ordinal").as_number();
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
  auto old = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U), Key(1U), {}, 10, garbage);
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
        Key(2U), old, 20, garbage) : old;
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
  const auto result = trace.SnapshotFile(ApplicationStartId{0x3f9a0000000000a1ULL},
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
  EXPECT_EQ(Ordinal(goals[0]), 2.0);
  EXPECT_EQ(Ordinal(goals[1]), 1.0);
  EXPECT_EQ(Member(Member(goals[0], "trajectory"), "points").as_array().size(), 8000U);
  EXPECT_EQ(Member(Member(goals[1], "trajectory"), "points").as_array().size(), 8000U);
  EXPECT_EQ(Member(Member(goals[0], "replacement_policy"), "schedule").as_string(), "controller_stamp");
  const auto &policy = Member(Member(goals[0], "replacement_policy"), "experiment");
  EXPECT_EQ(Member(policy, "release_hold").as_string(), "held");
  EXPECT_EQ(Member(policy, "factor").as_number(), 0.5);
  EXPECT_EQ(Member(snapshot, "scale_events").as_array().size(), 1U);
  VolatileMotionTrace restarted;
  const auto later = restarted.SnapshotFile(ApplicationStartId{0x3f9a0000000000b2ULL}, 4096U, directory);
  ASSERT_TRUE(later.ok()) << later.error().message;
  EXPECT_EQ(Read(metadata), bytes);
  EXPECT_EQ(Member(snapshot, "schema_version").as_string(), "manyforge.volatile_motion_trace.v2");
  EXPECT_EQ(Member(snapshot, "application_start_id").as_string(), "3f9a0000000000a1");
  EXPECT_EQ(Member(metadata, "schema_version").as_string(), "manyforge.motion_trace_file.v2");
  EXPECT_EQ(metadata.as_object().size(), 5U);
  EXPECT_EQ(Member(content_identity::parse_canonical_json(Read(later.value())),
                   "application_start_id").as_string(), "3f9a0000000000b2");
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
        Key(static_cast<std::uint32_t>(i) + 1U), {}, 0, garbage);
    auto goal = Goal();
    goal.trajectory.points.resize(1000U, goal.trajectory.points.front());
    trace.Attach(token, std::move(goal), 1, ExecutionPortDispatch::kDispatched, garbage);
  }
  const auto result = trace.SnapshotFile(ApplicationStartId{1U},
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
  const auto owner = ApplicationStartId{1U};
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
  const auto result = trace.SnapshotFile(ApplicationStartId{1U}, 4096U, directory);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "motion_trace_store_full");
  EXPECT_TRUE(std::filesystem::exists(stale));
}
TEST(VolatileMotionTraceTest, NegativeZeroGoalExportsWithoutChangingRetainedData) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  const auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                                 Key(1U), {}, 1, garbage);
  auto goal = Goal(-0.0);
  auto &point = goal.trajectory.points.front();
  point.velocities = {-0.0, -0.25};
  point.accelerations = {-0.0, 0.5};
  point.effort = {-0.0, 2.5};
  trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
               garbage);
  const auto result = trace.Snapshot(ApplicationStartId{1U}, 4000U);
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
  EXPECT_FALSE(trace.Snapshot(ApplicationStartId{1U}, 1U).ok());

  // Eviction exposes the retained payload: encoding must not rewrite it.
  for (std::size_t i = 0; i < VolatileMotionTrace::kGoalLimit; ++i)
    trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                Key(static_cast<std::uint32_t>(i) + 2U), {}, 3, garbage);
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
    const auto result = trace.Snapshot(ApplicationStartId{1U}, 4000U);
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
  const auto result = trace.Snapshot(ApplicationStartId{1U}, 4000U);
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
  const auto key = Key(1U);
  auto token =
      trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U), key, {}, 10, garbage);
  ASSERT_TRUE(token);
  ExecutionGoalObservationDTO facts;
  facts.key = key;
  facts.binding_epoch = ExecutionBindingEpoch(7U);
  facts.mechanism_goal_id = MechanismGoalId{{0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
                                              0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98,
                                              0x76, 0x54, 0x32, 0x10}};
  facts.accepted_stamp_ns = 11;
  facts.latest_feedback = ExecutionGoalFeedbackDTO{};
  facts.latest_feedback->local_receipt_stamp_ns = 12;
  trace.Observe(token, facts); // Callback before the SDK send returns.
  auto goal = Goal();
  trace.Attach(token, std::move(goal), 13, ExecutionPortDispatch::kDispatched,
               garbage);
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &row = Member(result.value(), "goals").as_array().front();
  // The 16 bytes are written as hex only in the export.
  EXPECT_EQ(Member(row, "mechanism_goal_id").as_string(),
            "0123456789abcdeffedcba9876543210");
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
          "/arm/fjt", ExecutionBindingEpoch(7U), Key(1U),
          VolatileMotionTrace::Context{OperationNumber(1U),
                                       OperationParticipantId{OperationNumber(1U), 1U},
                                       ProgramRunNumber(1U), "binding.actual",
                                       "robot.actual", 3U}});
  auto stop = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U),
                          Key(1U, 2U), parent, 10, garbage);
  auto newcomer = trace.Begin("/arm/fjt", ExecutionBindingEpoch(7U),
                              Key(2U), parent, 11, garbage);
  auto epoch = trace.Begin("/arm/fjt", ExecutionBindingEpoch(8U),
                           Key(1U, 2U), parent, 12, garbage);
  ASSERT_TRUE(stop);
  ASSERT_TRUE(newcomer);
  ASSERT_TRUE(epoch);
  ASSERT_TRUE(stop->context);
  EXPECT_EQ(stop->context->operation_id, OperationNumber(1U));
  EXPECT_FALSE(newcomer->context);
  EXPECT_FALSE(epoch->context);
}
TEST(VolatileMotionTraceTest,
     UnsignedIdentityBoundariesRemainExactAndDistinct) {
  constexpr std::uint64_t safe = 9007199254740991ULL;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  constexpr auto top = std::numeric_limits<std::uint32_t>::max();
  const auto exact = [&](std::uint32_t generation) {
    return ControllerGoalId{OperationNumber(safe), 1U, generation};
  };
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto parent =
      std::make_shared<VolatileMotionTrace::Token>(VolatileMotionTrace::Token{
          "/arm/fjt", ExecutionBindingEpoch(safe + 2U), exact(top - 2U),
          VolatileMotionTrace::Context{OperationNumber(safe),
                                       OperationParticipantId{OperationNumber(safe), 1U},
                                       ProgramRunNumber(safe), "binding.actual",
                                       "robot.actual", maximum}});
  auto first = trace.Begin("/arm/fjt", ExecutionBindingEpoch(safe + 2U),
                           exact(top - 1U), parent, 10, garbage);
  auto second = trace.Begin("/arm/fjt", ExecutionBindingEpoch(safe + 2U),
                            exact(top), first, 11, garbage);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  trace.Attach(first, Goal(), 12, ExecutionPortDispatch::kDispatched, garbage);
  trace.Attach(second, Goal(), 13, ExecutionPortDispatch::kDispatched, garbage);
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  const auto &rows = Member(result.value(), "goals").as_array();
  ASSERT_EQ(rows.size(), 2U);
  const auto generation = [](const V &row) {
    return Member(Member(row, "controller_goal_id"), "generation").as_number();
  };
  EXPECT_EQ(generation(rows[0]), static_cast<double>(top));
  EXPECT_EQ(generation(rows[1]), static_cast<double>(top - 1U));
  EXPECT_EQ(Member(Member(rows[0], "controller_goal_id"), "operation_id").as_number(),
            static_cast<double>(safe));
  EXPECT_EQ(Member(rows[0], "operation_id").as_number(), static_cast<double>(safe));
  EXPECT_EQ(Member(Member(rows[0], "participant_id"), "operation_id").as_number(),
            static_cast<double>(safe));
  EXPECT_EQ(Member(Member(rows[0], "participant_id"), "ordinal").as_number(), 1.0);
  EXPECT_EQ(Member(rows[0], "binding_epoch").as_string(), "9007199254740993");
  EXPECT_EQ(Member(rows[0], "operation_revision").as_string(),
            "18446744073709551615");
  EXPECT_EQ(generation(Member(rows[0], "predecessor")),
            static_cast<double>(top - 1U));
  EXPECT_EQ(generation(Member(rows[1], "predecessor")),
            static_cast<double>(top - 2U));
}
TEST(VolatileMotionTraceTest,
     EvictionDetachesPayloadAndCallbackTokenDoesNotPinIt) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  auto first = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U), Key(100U),
                           {}, 1, garbage);
  trace.Attach(first, Goal(), 2, ExecutionPortDispatch::kDispatched, garbage);
  for (std::size_t i = 0; i < VolatileMotionTrace::kGoalLimit; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             Key(static_cast<std::uint32_t>(i) + 1U),
                             {}, 3, garbage);
    trace.Attach(token, Goal(), 4, ExecutionPortDispatch::kDispatched, garbage);
  }
  ASSERT_EQ(garbage.size(), 1U);
  std::weak_ptr<const VolatileMotionTrace::Goal> retired = garbage.front();
  garbage.clear();
  EXPECT_TRUE(retired.expired());
  EXPECT_TRUE(first);
  const auto result = trace.Snapshot(ApplicationStartId{1U},
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
  auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U), Key(1U),
                           {}, 1, garbage);
  auto goal = Goal();
  goal.trajectory.points.resize(200U, goal.trajectory.points.front());
  trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
               garbage);
  EXPECT_FALSE(trace.Snapshot(ApplicationStartId{1U}, 1U).ok());
  const auto result = trace.Snapshot(ApplicationStartId{1U}, 2000U);
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
                             Key(static_cast<std::uint32_t>(i) + 1U),
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
                           Key(1000U), {}, 3, garbage));
  pins.clear();
  garbage.clear();
  auto resumed = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             Key(1001U), {}, 4, garbage);
  ASSERT_TRUE(resumed);
  trace.Attach(resumed, Goal(), 5, ExecutionPortDispatch::kDispatched, garbage);
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_GE(Member(Member(result.value(), "coverage"), "omitted_captures")
                .as_number(),
            2.0);
  EXPECT_EQ(Ordinal(Member(result.value(), "goals").as_array().front()), 1001.0);
}
TEST(VolatileMotionTraceTest,
     ResidentByteBoundEvictsAndOversizedCaptureHasNoPartialPoints) {
  VolatileMotionTrace trace;
  VolatileMotionTrace::Garbage garbage;
  for (int i = 0; i < 8; ++i) {
    auto token = trace.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                             Key(static_cast<std::uint32_t>(i) + 1U),
                             {}, 1, garbage);
    auto goal = Goal();
    goal.trajectory.points.resize(5000U, goal.trajectory.points.front());
    trace.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
                 garbage);
    garbage.clear();
  }
  const auto bounded = trace.Snapshot(ApplicationStartId{1U}, 2000U);
  ASSERT_TRUE(bounded.ok());
  EXPECT_GT(
      Member(Member(bounded.value(), "coverage"), "evicted_goals").as_number(),
      0.0);

  VolatileMotionTrace oversize;
  auto token = oversize.Begin("/arm/fjt", ExecutionBindingEpoch(1U),
                              Key(1U), {}, 1, garbage);
  auto goal = Goal();
  goal.trajectory.points.front().positions.resize(
      VolatileMotionTrace::kGoalBytes / sizeof(double) + 1U);
  oversize.Attach(token, std::move(goal), 2, ExecutionPortDispatch::kDispatched,
                  garbage);
  const auto result = oversize.Snapshot(ApplicationStartId{1U}, 2000U);
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
  for (std::size_t i = 0; i < VolatileMotionTrace::kSampleCapacity + 3U; ++i) {
    sample.local_receipt_stamp_ns = static_cast<TimePoint>(i);
    trace.Sample("/arm/fjt", sample);
  }
  trace.PublishedScale("/arm/fjt", ExecutionBindingEpoch(1U), 2000, 0.25);
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(Member(result.value(), "controller_samples").as_array().size(),
            VolatileMotionTrace::kSampleCapacity);
  EXPECT_GE(
      Member(Member(result.value(), "coverage"), "dropped_samples").as_number(),
      3.0);
  const auto &events = Member(result.value(), "scale_events").as_array();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(Member(events.front(), "kind").as_string(), "published");
  EXPECT_EQ(Member(events.back(), "kind").as_string(), "applied_observed");
}
// The encoding of a retained `ExecutionControllerSampleDTO` before samples
// became compact rows, kept verbatim as the export reference.
V ReferenceNumber(double value) { return V(value == 0.0 ? 0.0 : value); }
V ReferenceNs(TimePoint n) {
  return n >= 0 ? content_identity::nonnegative_int64_value(n)
                : V(std::to_string(n));
}
V ReferenceCount(std::uint64_t n) {
  return n <= 9007199254740991ULL ? V(static_cast<double>(n))
                                  : V(std::to_string(n));
}
V ReferenceNumbers(const std::vector<double> &input) {
  V::Array out;
  for (double value : input)
    out.emplace_back(ReferenceNumber(value));
  return V(std::move(out));
}
V ReferenceRow(const std::string &action, const ExecutionControllerSampleDTO &s) {
  return V(V::Object{
      {"action", V(action)},
      {"binding_epoch", ReferenceCount(s.binding_epoch.value())},
      {"source_stamp_ns", ReferenceNs(s.source_stamp_ns)},
      {"local_receipt_stamp_ns", ReferenceNs(s.local_receipt_stamp_ns)},
      {"cursor_s", ReferenceNumber(s.cursor_s)},
      {"reference_p", ReferenceNumbers(s.reference_positions_rad)},
      {"reference_v", ReferenceNumbers(s.reference_velocities_rad_s)},
      {"reference_a", ReferenceNumbers(s.reference_accelerations_rad_s2)},
      {"commanded_p", ReferenceNumbers(s.command_positions_rad)},
      {"commanded_v", s.command_velocities_rad_s
                          ? ReferenceNumbers(*s.command_velocities_rad_s)
                          : V(nullptr)},
      {"measured_p", ReferenceNumbers(s.measured_positions_rad)},
      {"measured_v", s.measured_velocities_rad_s
                         ? ReferenceNumbers(*s.measured_velocities_rad_s)
                         : V(nullptr)},
      {"applied_factor",
       s.applied_factor ? ReferenceNumber(*s.applied_factor) : V(nullptr)}});
}
std::vector<double> Joints(std::size_t count, double base) {
  std::vector<double> values;
  for (std::size_t i = 0; i < count; ++i)
    values.push_back(base + static_cast<double>(i) * 0.1234567890123);
  return values;
}
ExecutionControllerSampleDTO VariedSample(std::size_t i) {
  const std::size_t joints = i % 3U == 0U ? 7U : 6U;
  ExecutionControllerSampleDTO s;
  s.binding_epoch = ExecutionBindingEpoch(i % 5U == 0U ? 9007199254740993ULL : 3U);
  s.source_stamp_ns = static_cast<TimePoint>(i) * 6666667 - 3;
  s.local_receipt_stamp_ns = 1700000000123456789LL + static_cast<TimePoint>(i);
  s.local_receipt_count = ExecutionReceiptCount(i + 1U);
  s.cursor_s = i % 4U == 0U ? -0.0 : static_cast<double>(i) / 3.0;
  if (i % 2U == 0U)
    s.applied_factor = i % 6U == 0U ? -0.0 : 0.1 * static_cast<double>(i % 10U);
  s.reference_positions_rad = Joints(joints, -1.0 / 3.0 - static_cast<double>(i));
  s.reference_velocities_rad_s = Joints(joints, 1e-300);
  if (i % 7U != 0U)
    s.reference_accelerations_rad_s2 = Joints(joints, -2.5e7);
  s.command_positions_rad = Joints(joints, 0.7);
  s.reference_positions_rad.front() = -0.0;
  if (i % 3U == 1U)
    s.command_velocities_rad_s = Joints(joints, -0.25);
  else if (i % 3U == 2U)
    s.command_velocities_rad_s = std::vector<double>{};
  s.measured_positions_rad = Joints(joints, std::numeric_limits<double>::min());
  if (i % 2U == 1U)
    s.measured_velocities_rad_s = Joints(joints, std::numeric_limits<double>::max() / 2.0);
  s.error_positions_rad = Joints(joints, 0.001);
  return s;
}
TEST(VolatileMotionTraceTest, CompactSamplesExportLikeTheRetainedSampleEncoding) {
  VolatileMotionTrace trace;
  std::vector<std::pair<std::string, ExecutionControllerSampleDTO>> captured;
  for (std::size_t i = 0; i < 60U; ++i) {
    captured.emplace_back(i % 2U == 0U ? "/left/fjt" : "/right_arm_controller/follow_joint_trajectory",
                          VariedSample(i));
    trace.Sample(captured.back().first, captured.back().second);
  }
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &rows = Member(result.value(), "controller_samples").as_array();
  ASSERT_EQ(rows.size(), captured.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &[action, sample] = captured[captured.size() - 1U - i];
    EXPECT_EQ(content_identity::canonical_json(rows[i]),
              content_identity::canonical_json(ReferenceRow(action, sample)))
        << i;
  }
  const auto &coverage = Member(result.value(), "coverage");
  EXPECT_EQ(Member(coverage, "capture_failures").as_number(), 0.0);
  EXPECT_EQ(Member(coverage, "dropped_samples").as_number(), 0.0);
  EXPECT_EQ(Member(Member(result.value(), "retention"), "sample_limit").as_number(),
            static_cast<double>(VolatileMotionTrace::kSampleCapacity));
}
TEST(VolatileMotionTraceTest, OptionalVectorsKeepAbsentApartFromEmpty) {
  VolatileMotionTrace trace;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.reference_positions_rad = {0.5};
  sample.command_velocities_rad_s = std::vector<double>{};
  sample.measured_velocities_rad_s = std::nullopt;
  trace.Sample("/arm/fjt", sample);
  const auto result = trace.Snapshot(ApplicationStartId{1U}, 4000U);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &row = Member(result.value(), "controller_samples").as_array().front();
  EXPECT_TRUE(Member(row, "commanded_v").is_array());
  EXPECT_TRUE(Member(row, "commanded_v").as_array().empty());
  EXPECT_TRUE(Member(row, "measured_v").is_null());
  EXPECT_TRUE(Member(row, "reference_a").as_array().empty());
  EXPECT_TRUE(Member(row, "applied_factor").is_null());
  EXPECT_EQ(content_identity::canonical_json(row),
            content_identity::canonical_json(ReferenceRow("/arm/fjt", sample)));
}
TEST(VolatileMotionTraceTest, RingKeepsArrivalOrderAndEvictsOldestFirst) {
  static_assert(VolatileMotionTrace::kSampleCapacity >= 2000U);
  static_assert(VolatileMotionTrace::kTraceJoints >= 7U);
  VolatileMotionTrace trace;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.reference_positions_rad = Joints(6U, 0.0);
  constexpr std::size_t extra = 5U;
  const auto total = VolatileMotionTrace::kSampleCapacity + extra;
  for (std::size_t i = 0; i < total; ++i) {
    sample.local_receipt_stamp_ns = static_cast<TimePoint>(i);
    trace.Sample(i < extra ? "/old/fjt" : "/arm/fjt", sample);
  }
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &rows = Member(result.value(), "controller_samples").as_array();
  ASSERT_EQ(rows.size(), VolatileMotionTrace::kSampleCapacity);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(Member(rows[i], "local_receipt_stamp_ns").as_number(),
              static_cast<double>(total - 1U - i));
    EXPECT_EQ(Member(rows[i], "action").as_string(), "/arm/fjt");
  }
  EXPECT_EQ(Member(Member(result.value(), "coverage"), "dropped_samples").as_number(),
            static_cast<double>(extra));
}
TEST(VolatileMotionTraceTest, OversizedSamplesAndUnknownActionsAreCountedNotKept) {
  VolatileMotionTrace trace;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.reference_positions_rad = Joints(VolatileMotionTrace::kTraceJoints + 1U, 0.0);
  trace.Sample("/arm/fjt", sample);
  sample.reference_positions_rad = Joints(1U, 0.0);
  sample.measured_velocities_rad_s = Joints(VolatileMotionTrace::kTraceJoints + 1U, 0.0);
  trace.Sample("/arm/fjt", sample);
  sample.measured_velocities_rad_s.reset();
  trace.Sample(std::string(VolatileMotionTrace::kActionBytes + 1U, 'a'), sample);
  // Sixteen distinct retained actions fill the identity table.
  for (std::size_t i = 0; i < 17U; ++i)
    trace.Sample("/arm" + std::to_string(i) + "/fjt", sample);
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(Member(result.value(), "controller_samples").as_array().size(), 16U);
  EXPECT_EQ(Member(Member(result.value(), "coverage"), "capture_failures").as_number(), 4.0);
  // Once the ring no longer names an action its slot is reused.
  for (std::size_t i = 0; i < VolatileMotionTrace::kSampleCapacity; ++i)
    trace.Sample("/arm0/fjt", sample);
  trace.Sample("/arm16/fjt", sample);
  const auto later = trace.Snapshot(ApplicationStartId{1U}, 4000U);
  ASSERT_TRUE(later.ok()) << later.error().message;
  EXPECT_EQ(Member(Member(later.value(), "controller_samples").as_array().front(),
                   "action").as_string(), "/arm16/fjt");
  EXPECT_EQ(Member(Member(later.value(), "coverage"), "capture_failures").as_number(), 4.0);
}
TEST(VolatileMotionTraceTest, SampleCaptureNeitherAllocatesNorFrees) {
  VolatileMotionTrace trace;
  ExecutionControllerSampleDTO sample;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.reference_positions_rad = Joints(7U, 0.1);
  sample.reference_velocities_rad_s = Joints(7U, 0.2);
  sample.reference_accelerations_rad_s2 = Joints(7U, 0.3);
  sample.command_positions_rad = Joints(7U, 0.4);
  sample.command_velocities_rad_s = Joints(7U, 0.5);
  sample.measured_positions_rad = Joints(7U, 0.6);
  sample.measured_velocities_rad_s = Joints(7U, 0.7);
  sample.error_positions_rad = Joints(7U, 0.8);
  const std::string left = "/left_arm_controller/follow_joint_trajectory";
  const std::string right = "/right_arm_controller/follow_joint_trajectory";
  // First use of each action, ring wrap, scale-event ring wrap and published
  // scale events all inside the counted window.
  const auto calls = HeapCallsDuring([&] {
    for (std::size_t i = 0; i < VolatileMotionTrace::kSampleCapacity + 300U; ++i) {
      sample.local_receipt_stamp_ns = static_cast<TimePoint>(i);
      sample.applied_factor = static_cast<double>((i / 2U) % 2U);
      trace.Sample(i % 2U == 0U ? left : right, sample);
      if (i % 3U == 0U)
        trace.PublishedScale(left, ExecutionBindingEpoch(1U), static_cast<TimePoint>(i), 0.5);
    }
  });
  EXPECT_EQ(calls, 0U);
  const auto result = trace.Snapshot(ApplicationStartId{1U},
                                     VolatileMotionTrace::kMaximumTraceBytes);
  ASSERT_TRUE(result.ok()) << result.error().message;
  const auto &coverage = Member(result.value(), "coverage");
  EXPECT_EQ(Member(coverage, "dropped_samples").as_number(), 300.0);
  EXPECT_GT(Member(coverage, "evicted_scale_events").as_number(), 0.0);
  EXPECT_EQ(Member(coverage, "capture_failures").as_number(), 0.0);
}
} // namespace
} // namespace manyforge::ros2_control_execution
