// Actual retained handle, participant, addressed Kernel and SQLite journal;
// the ROS controller and prepared planning engine are existing test peers.
#include "test_ros2_control_kernel_stop_integration.cpp"
#include "retired_composition_release.hpp"
#include <manyforge_ros2_control_execution/detail/leaf_timing.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <future>

namespace manyforge::roboplan_backend {
class RetainedRos2ControlRuntimeHandleTestAccess final {
 public:
  static auto EpochResources(const RetainedRos2ControlRuntimeHandle &handle) {
    return handle.TestEpochResources();
  }
};
namespace {
enum class ShutdownCase { kStuck, kHandoffFailure, kTimelyRelease, kRetiredRetry, kSnapshotRetry, kDuplicateSnapshot };
void RunRetainedMotionLeafShutdown(ShutdownCase scenario) {
  namespace timing = manyforge::ros2_control_execution::detail;
  auto clock_owner = std::make_shared<WallClock>();
  auto &clock = *clock_owner;
  SimpleScheduler scheduler;
  const auto binding = KernelBinding();
  auto readiness = StopReadiness(binding, ExpectedDomain(binding));
  const std::string prefix = "/manyforge_leaf_shutdown_" + fake::unique_suffix();
  fake::FakeJtc jtc(prefix + "/arm_controller", {"source_joint_1"});
  fake::FakeControllerManager manager(prefix + "/controller_manager", "arm_controller", "TestSystem", &jtc);
  core::ObservedRobotStatePublication publication{
      {"joint_1"}, {core::ObservedRobotStateSourceConfig{
          core::ObservedRobotStateSourceKind::kCommandedState, "commanded_state",
          core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0}}};
  RetainedRos2ControlArmTransport arm;
  arm.adapter = AdapterConfig(prefix + "/arm_controller");
  if (scenario == ShutdownCase::kRetiredRetry || scenario == ShutdownCase::kSnapshotRetry || scenario == ShutdownCase::kDuplicateSnapshot)
    arm.adapter.lifecycle_command_timeout_ns = 200'000'000;
  arm.controller = {prefix + "/controller_manager", "arm_controller", "TestSystem"};
  arm.compiled_joint_names = {"joint_1"};
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "manyforge_leaf_shutdown_" + fake::unique_suffix(), "/", 3U,
      std::make_shared<StateSink>(), "arm", clock);
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  ASSERT_TRUE(handle->Bind(owner, clock, scheduler, arm,
      [&](std::shared_ptr<core::IExecutionPort> port) {
        return std::make_shared<core::ExecutionPortParticipant>(
            binding, std::move(port), clock, scheduler, SamplePeriodWaiter(clock),
            core::ExecutionPortParticipantStateAuthority{&publication, [] { return core::Result<void>::Ok(); }},
            ParticipantConfig());
      }, nullptr, binding, std::nullopt, clock_owner).ok());
  handle->StartBackend();
  const auto epoch_owner = RetainedRos2ControlRuntimeHandleTestAccess::EpochResources(*handle);
  ASSERT_TRUE(epoch_owner);
  auto participant_owner = handle->participant();
  ASSERT_TRUE(participant_owner);
  auto &participant = *participant_owner;
  RecordingRos2ControlStopPort stop_port(participant);
  Ros2ControlPreparedMotionEngine engine(participant);
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto activity = handle->arm_activity();
    return activity.observed && activity.controller_loaded && activity.hardware_active;
  }));
  ASSERT_EQ(handle->PrepareArmController({}, Soon()).outcome, RetainedRos2ControlSwitchOutcome::kSucceeded);
  ASSERT_EQ(handle->ActivateArmController({}, Soon()).outcome, RetainedRos2ControlSwitchOutcome::kSucceeded);
  const auto port = handle->current_port();
  ASSERT_TRUE(fake::WaitUntil([&] {
    const auto state = port->ReadReadiness();
    return state.controller_active && state.controller_bound && state.hardware_active && port->ReadSample().has_value();
  }));
  const auto journal_path = std::filesystem::temp_directory_path() /
      ("manyforge-retained-leaf-" + fake::unique_suffix() + ".sqlite");
  struct RemoveJournal {
    std::filesystem::path path;
    ~RemoveJournal() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + "-wal", ignored);
      std::filesystem::remove(path.string() + "-shm", ignored);
    }
  } remove_journal{journal_path};
  core::SqliteOperationJournal journal(journal_path, core::CellId(kCellId), 16U);
  auto kernel_owner = core::testing::make_heap_resource_addressed_kernel(
      {Registration(binding, std::move(readiness), engine, stop_port)}, clock, scheduler, journal,
      core::ResourceAddressedExecutionKernelConfig{core::RequestOwnerEpoch(kOwnerEpoch), core::CellId(kCellId), 16U});
  auto &kernel = *kernel_owner;
  std::atomic<bool> handoff_succeeded{false};
  std::atomic<bool> owner_joined_during_handoff{true};
  std::string handoff_error_code;
  std::atomic<bool> inject_handoff_failure{scenario == ShutdownCase::kHandoffFailure};
  struct HandleCleanup {
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> &handle;
    ~HandleCleanup() { handle.reset(); }
  } cleanup{handle};
  handle->InstallLateCancelJournalHook(&engine, [&](std::uint64_t execution_id) {
    owner_joined_during_handoff = owner->joined() || epoch_owner->joined();
    auto result = kernel.record_late_cancel_uncertain(
        inject_handoff_failure.load() ? nullptr : &engine, execution_id);
    if (result.ok()) handoff_succeeded = true;
    else handoff_error_code = result.error().code;
    return result;
  });
  timing::goal_admitted_entered = false;
  timing::goal_admitted_timed_out = false;
  timing::release_goal_admitted = false;
  timing::block_goal_admitted = true;
  using ExecutionResult = decltype(kernel.execute(MotionOperation(binding, "leaf-shutdown")));
  std::future<ExecutionResult> execution;
  struct Release { ~Release() { timing::release_goal_admitted = true; timing::block_goal_admitted = false; } } release;
  execution = std::async(std::launch::async, [&] { return kernel.execute(MotionOperation(binding, "leaf-shutdown")); });
  const bool entered = fake::WaitUntil([] { return timing::goal_admitted_entered.load(); });
  RetainedRos2ControlRuntimeHandle::HostQuiesceResult stopped{
      RetainedRos2ControlRuntimeHandle::HostQuiesceOutcome::kUnresolved, "not_entered", ""};
  bool close_fenced = false;
  std::unique_ptr<host_detail::Composition> retired;
  std::unique_ptr<host_detail::Composition> active, prepared, failed;
  std::set<core::IResolvedParticipantExecutionEngine *> retired_engines;
  const host_detail::Composition *logged = nullptr;
  std::mutex composition_mutex;
  int reclaimed = 0;
  auto release_retired = [&] {
    host_detail::TryReleaseRetiredComposition(
        composition_mutex, active, prepared, failed, retired, retired_engines, logged,
        [&] { return &kernel; },
        [](const host_detail::Composition &) {
          return std::set<core::IResolvedParticipantExecutionEngine *>{};
        },
        [](const host_detail::Composition &v) {
          return std::vector<std::pair<int, const host_detail::Composition *>>{{0, &v}};
        },
        [](const auto &) { ADD_FAILURE() << "unexpected controller retirement"; return true; },
        [&] { ++reclaimed; },
        [](bool ok, const char *message) { if (!ok) throw std::runtime_error(message); });
  };
  std::vector<host_detail::PendingDiscard> snapshot_pending;
  const auto registrations = [](const host_detail::Composition &) {
    return std::set<core::IResolvedParticipantExecutionEngine *>{};
  };
  const auto collect_pending = [&](const host_detail::PendingDiscard &p, auto &pins, auto &engines) {
    host_detail::CollectRetainedPendingProtection(p, pins, engines, registrations);
  };
  const auto retry_snapshots = [&] {
    std::vector<host_detail::PendingDiscard> batch;
    batch.swap(snapshot_pending);
    host_detail::SettleApplyPendingSnapshots(
        batch, snapshot_pending, {nullptr, nullptr, nullptr, nullptr}, [&] { return &kernel; },
        registrations, collect_pending);
    return snapshot_pending.empty();
  };
  bool retained_same_slot_twice = false;
  std::size_t leaf_sample_start = 0;
  std::string retirement_logs;
  if (entered && (scenario == ShutdownCase::kSnapshotRetry ||
                  scenario == ShutdownCase::kDuplicateSnapshot)) {
    // Planning-product fields are unused by the cleanup helper; this fixture
    // tests the actual typed snapshot and its genuinely bound handle.
    host_detail::RetainedComponentSnapshot snapshot{
        nullptr, nullptr, nullptr,
        {core::ProductId("fixture-product"), core::RetainedProductKind::kPlanningCollisionModel,
         core::ProductContractId("fixture-contract"), core::ProductContractRevision("fixture-revision"), {}},
        std::nullopt, host_detail::CanonicalValue(nullptr), host_detail::CanonicalValue(nullptr), handle, nullptr};
    ::testing::internal::CaptureStderr();
    if (scenario == ShutdownCase::kDuplicateSnapshot) {
      std::vector<host_detail::PendingDiscard> local;
      local.push_back(host_detail::PendingDiscard{nullptr, {snapshot}, "retired-arm", false});
      local.push_back(host_detail::PendingDiscard{nullptr, {std::move(snapshot)}, "retired-arm-2", false});
      leaf_sample_start = timing::leaf_sample_count.load();
      host_detail::SettleApplyPendingSnapshots(
          local, snapshot_pending, {nullptr, nullptr, nullptr, nullptr}, [&] { return &kernel; },
          registrations, collect_pending);
      retained_same_slot_twice = local.size() == 2U && snapshot_pending.size() == 2U &&
          snapshot_pending[0].snapshots.front().ros2_control_runtime_handle == handle &&
          snapshot_pending[1].snapshots.front().ros2_control_runtime_handle == handle;
    } else {
      snapshot_pending.push_back(host_detail::PendingDiscard{nullptr, {std::move(snapshot)}, "retired-arm", false});
      const bool first_failed = !retry_snapshots();
      const bool same_pin_first = snapshot_pending.size() == 1U &&
          snapshot_pending.front().snapshots.front().ros2_control_runtime_handle == handle;
      const bool second_failed = !retry_snapshots();
      retained_same_slot_twice = first_failed && same_pin_first && second_failed &&
          snapshot_pending.size() == 1U && snapshot_pending.front().snapshots.front().ros2_control_runtime_handle == handle;
    }
    retirement_logs = ::testing::internal::GetCapturedStderr();
  } else if (entered && scenario == ShutdownCase::kRetiredRetry) {
    retired = std::make_unique<host_detail::Composition>(
        core::ControllableComponentId("retired-arm"), nullptr, nullptr, nullptr, nullptr, nullptr,
        std::nullopt, std::nullopt, std::nullopt, host_detail::CanonicalValue(nullptr),
        host_detail::CanonicalValue(nullptr), nullptr, nullptr, core::ProjectionRevisionDTO{},
        core::ActiveBundlePublicationGeneration(1U), std::nullopt, std::nullopt);
    retired->ros2_control_runtime_handle = handle;
    const auto *original = retired.get();
    ::testing::internal::CaptureStderr();
    release_retired();
    const bool first_retained = retired.get() == original && logged == original;
    release_retired();
    retained_same_slot_twice = first_retained && retired.get() == original && logged == original;
    retirement_logs = ::testing::internal::GetCapturedStderr();
  } else if (entered && scenario == ShutdownCase::kTimelyRelease) {
    auto closing = std::async(std::launch::async, [&] {
      return handle->QuiesceForHostShutdown(std::chrono::steady_clock::now() + 3s);
    });
    close_fenced = fake::WaitUntil([&] { return !port->ReadReadiness().controller_active; });
    timing::release_goal_admitted = true;
    stopped = closing.get();
  } else if (entered) {
    stopped = handle->QuiesceForHostShutdown(std::chrono::steady_clock::now() + 200ms);
  }
  const bool owner_retained = !owner->joined() && owner->cached_context_valid() &&
                              !epoch_owner->joined() && epoch_owner->cached_context_valid();
  timing::release_goal_admitted = true;
  const bool execution_returned = execution.wait_for(3s) == std::future_status::ready;
  std::optional<core::OperationId> operation_id;
  if (execution_returned) {
    const auto result = execution.get();
    if (result.ok()) operation_id = result.value().operation_id;
  }
  EXPECT_TRUE(entered) << "actual handle did not enter the instrumented action leaf";
  EXPECT_FALSE(timing::goal_admitted_timed_out.load());
  if (scenario == ShutdownCase::kRetiredRetry || scenario == ShutdownCase::kSnapshotRetry ||
      scenario == ShutdownCase::kDuplicateSnapshot) {
    EXPECT_TRUE(retained_same_slot_twice);
    EXPECT_TRUE(owner_retained);
    EXPECT_TRUE(handoff_succeeded.load());
    EXPECT_FALSE(owner_joined_during_handoff.load());
    const auto first = retirement_logs.find("\"component\":\"retired-arm\"");
    EXPECT_NE(first, std::string::npos);
    if (first != std::string::npos) {
      EXPECT_EQ(retirement_logs.find("\"component\":\"retired-arm\"", first + 1),
                std::string::npos);
    }
    EXPECT_NE(retirement_logs.find("\"code\":\"component_owner_stalled\""), std::string::npos);
    if (scenario == ShutdownCase::kRetiredRetry) {
      EXPECT_NE(retirement_logs.find("\"generation\":1"), std::string::npos);
    }
    EXPECT_NE(retirement_logs.find("\"stage\":\"leaf\""), std::string::npos);
    EXPECT_TRUE(execution_returned);
    if (scenario == ShutdownCase::kRetiredRetry) {
      release_retired();
      EXPECT_FALSE(retired);
      EXPECT_EQ(logged, nullptr);
      EXPECT_EQ(reclaimed, 1);
    } else {
      EXPECT_TRUE(retry_snapshots());
      EXPECT_TRUE(snapshot_pending.empty());
    }
  }
  if (scenario == ShutdownCase::kHandoffFailure) {
    EXPECT_EQ(stopped.outcome, RetainedRos2ControlRuntimeHandle::HostQuiesceOutcome::kUnresolved);
    EXPECT_STREQ(stopped.stage, "journal_handoff");
    EXPECT_FALSE(handoff_error_code.empty());
    EXPECT_EQ(stopped.code, handoff_error_code);
    EXPECT_FALSE(handoff_succeeded.load());
    EXPECT_FALSE(owner_joined_during_handoff.load());
    EXPECT_TRUE(owner_retained);
    // Restore the exact live Kernel handoff before teardown; the deliberately
    // unmapped engine above tests refusal propagation, not durable success.
    inject_handoff_failure = false;
    EXPECT_TRUE(execution_returned);
    return;
  }
  if (scenario == ShutdownCase::kTimelyRelease) {
    EXPECT_TRUE(close_fenced);
    EXPECT_EQ(stopped.outcome, RetainedRos2ControlRuntimeHandle::HostQuiesceOutcome::kSettled);
    EXPECT_TRUE(owner->joined());
    EXPECT_TRUE(epoch_owner->joined());
  } else if (scenario == ShutdownCase::kStuck) {
    EXPECT_EQ(stopped.outcome, RetainedRos2ControlRuntimeHandle::HostQuiesceOutcome::kUnresolved);
    EXPECT_STREQ(stopped.stage, "leaf");
    EXPECT_EQ(stopped.code, "component_owner_stalled");
    EXPECT_TRUE(owner_retained);
  }
  EXPECT_TRUE(handoff_succeeded.load());
  EXPECT_FALSE(owner_joined_during_handoff.load());
  EXPECT_TRUE(execution_returned);
  ASSERT_TRUE(operation_id.has_value());
  const auto state = kernel.get_operation_state(*operation_id);
  ASSERT_TRUE(state.ok());
  ASSERT_EQ(state.value().participants.size(), 1U);
  const auto participant_id = state.value().participants.front().participant_id.value();
  const auto events = journal.ReadEvents(core::RequestOwnerEpoch(kOwnerEpoch), 0U);
  ASSERT_TRUE(events.ok());
  EXPECT_TRUE(std::any_of(events.value().events.begin(), events.value().events.end(), [&](const auto &row) {
    if (!row.operation_id || *row.operation_id != *operation_id) return false;
    const auto payload = manyforge::content_identity::parse_canonical_json(row.payload_json);
    const auto *operation = payload.find_object_member("operation_state");
    const auto *participants = operation ? operation->find_object_member("participants") : nullptr;
    if (!participants || !participants->is_array()) return false;
    return std::any_of(participants->as_array().begin(), participants->as_array().end(), [&](const auto &p) {
      const auto *id = p.find_object_member("participant_id");
      const auto *fact = p.find_object_member("terminal_fact");
      const auto *effect = fact ? fact->find_object_member("effect") : nullptr;
      return id && id->is_string() && id->as_string() == participant_id &&
             effect && effect->is_string() && effect->as_string() == "effect_uncertain";
    });
  }));
  handle.reset(); // Kernel/journal hook remain live during final-owner cleanup.
  if (scenario == ShutdownCase::kDuplicateSnapshot) {
    // Destroy the addressed Kernel and its independent Stop/operation workers
    // before reading the buffer; then drain the separate scheduler.
    kernel_owner.reset();
    const bool owners_joined = execution_returned && owner->joined() && epoch_owner->joined();
    EXPECT_TRUE(owners_joined);
    bool scheduler_drained = false;
    if (owners_joined) {
      scheduler.close_admission_and_discard();
      scheduler_drained = scheduler.drain_all_for(std::chrono::seconds(2));
      EXPECT_TRUE(scheduler_drained);
    }
    if (owners_joined && scheduler_drained) {
      std::lock_guard samples_lock(timing::leaf_samples_mutex);
      const auto sample_end = timing::leaf_sample_count.load();
      ASSERT_LE(sample_end, timing::kLeafSampleCapacity);
      std::size_t barrier_attempts = 0;
      for (auto i = leaf_sample_start; i < sample_end; ++i)
        if (timing::leaf_samples[i].entity == timing::LeafEntity::kMotionStopBarrier)
          ++barrier_attempts;
      EXPECT_EQ(barrier_attempts, 2U); // failed shared settle + successful retry
    }
  }
}
TEST(RetainedMotionLeafTiming, StuckActionLeafRetainsOwnersAfterDurableUnknown) {
  RunRetainedMotionLeafShutdown(ShutdownCase::kStuck);
}
TEST(RetainedMotionLeafTiming, FailedHandoffRetainsOwners) {
  RunRetainedMotionLeafShutdown(ShutdownCase::kHandoffFailure);
}
TEST(RetainedMotionLeafTiming, TimelyLeafReleaseHandsOffBeforeJoiningOwners) {
  RunRetainedMotionLeafShutdown(ShutdownCase::kTimelyRelease);
}
TEST(RetainedMotionLeafTiming, RetiredBlockedOwnerKeepsSlotAndLogsOnceThenRetries) {
  RunRetainedMotionLeafShutdown(ShutdownCase::kRetiredRetry);
}
TEST(RetainedMotionLeafTiming, SnapshotOnlyBlockedOwnerKeepsPinAndLogsOnceThenRetries) {
  RunRetainedMotionLeafShutdown(ShutdownCase::kSnapshotRetry);
}
TEST(RetainedMotionLeafTiming, DuplicateLocalSnapshotsSettleSharedOwnerOnce) {
  RunRetainedMotionLeafShutdown(ShutdownCase::kDuplicateSnapshot);
}
} // namespace
} // namespace manyforge::roboplan_backend
