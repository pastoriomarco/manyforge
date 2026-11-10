#include "retired_composition_release.hpp"
#include <manyforge_core/testing/heap_resource_addressed_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_roboplan/execution_host_support.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <string>

namespace manyforge::roboplan_backend {
// The existing handle test peer observes the real close state under its mutex.
class RetainedRos2ControlRuntimeHandleTestAccess final {
 public:
  static bool Closing(const RetainedRos2ControlRuntimeHandle &handle) {
    std::lock_guard lock(handle.mutex_);
    return handle.closing_;
  }
  static bool ShutdownComplete(const RetainedRos2ControlRuntimeHandle &handle) {
    std::lock_guard lock(handle.mutex_);
    return handle.shutdown_complete_;
  }
};
namespace host_detail {
namespace {

// Real Composition trees use the same constructor as the host-composition
// ownership tests; runtime owners are installed explicitly by each case.
std::unique_ptr<Composition> View(const std::string &component) {
  return std::make_unique<Composition>(
      core::ControllableComponentId(component), nullptr, nullptr, nullptr, nullptr, nullptr,
      std::nullopt, std::nullopt, std::nullopt, CanonicalValue(nullptr), CanonicalValue(nullptr),
      nullptr, nullptr, core::ProjectionRevisionDTO{}, core::ActiveBundlePublicationGeneration(1U),
      std::nullopt, std::nullopt);
}

// Empty registration inventory is valid for these handle-only compositions;
// no execution engine or controller process is installed in this fixture.
// An empty-runtime Kernel must carry its exact bootstrap graph identity.
inline core::ResourceAddressedExecutionKernelConfig retirement_kernel_config() {
  core::ResourceAddressedExecutionKernelConfig config{
      core::RequestOwnerEpoch("retired-test-epoch"), core::CellId("retired-test"), 16U};
  config.empty_runtime_retained_bundle_id = core::RetainedBundleId("retired-test-bundle");
  config.empty_runtime_graph_revision = core::GraphRevision("retired-test-graph");
  return config;
}

struct RetirementFixture : ::testing::Test {
  WallClock clock;
  SimpleScheduler scheduler;
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("manyforge-retired-" + std::to_string(::getpid()) + ".sqlite");
  core::SqliteOperationJournal journal{path, core::CellId("retired-test"), 16U};
  std::unique_ptr<core::ResourceAddressedExecutionKernel> kernel =
      core::testing::make_heap_resource_addressed_kernel(
          {}, clock, scheduler, journal, retirement_kernel_config());
  std::mutex mutex;
  std::unique_ptr<Composition> active, prepared, failed, retired;
  std::set<core::IResolvedParticipantExecutionEngine *> retired_engines;
  const Composition *logged = nullptr;
  int reclaimed = 0;

  void Release() {
    TryReleaseRetiredComposition(
        mutex, active, prepared, failed, retired, retired_engines, logged,
        [&] { return kernel.get(); },
        [](const Composition &view) {
          EXPECT_FALSE(view.runtime);
          EXPECT_FALSE(view.gripper_runtime);
          return std::set<core::IResolvedParticipantExecutionEngine *>{};
        },
        [](const Composition &view) {
          // These cases carry no controller process: only the root is needed.
          EXPECT_FALSE(view.controller_process);
          return std::vector<std::pair<int, const Composition *>>{{0, &view}};
        },
        [](const auto &) { ADD_FAILURE() << "unexpected controller retirement"; return true; },
        [&] { ++reclaimed; },
        [](bool condition, const char *message) { if (!condition) throw std::runtime_error(message); });
  }
  ~RetirementFixture() override {
    // Keep a retained test handle destructible if an assertion exits early.
    if (retired && retired->ros2_control_runtime_handle)
      static_cast<void>(retired->ros2_control_runtime_handle->SetOwnerShutdownTimeout(1'000'000'000));
    kernel.reset();
    journal.Close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + "-wal", ec);
    std::filesystem::remove(path.string() + "-shm", ec);
  }
};

TEST_F(RetirementFixture, ProtectedRootsNeverCloseTheSharedHandle) {
  for (int root = 0; root < 3; ++root) {
    auto handle = RetainedRos2ControlRuntimeHandle::Create();
    ASSERT_TRUE(handle->SetOwnerShutdownTimeout(1'000'000'000).ok());
    retired = View("retired");
    retired->ros2_control_runtime_handle = handle;
    auto protected_view = View("protected");
    protected_view->ros2_control_runtime_handle = handle;
    auto *slot = root == 0 ? &active : root == 1 ? &prepared : &failed;
    *slot = std::move(protected_view);
    Release();
    EXPECT_FALSE(retired);
    EXPECT_FALSE(RetainedRos2ControlRuntimeHandleTestAccess::Closing(*handle));
    slot->reset();
  }
  EXPECT_EQ(reclaimed, 3);
}

TEST_F(RetirementFixture, FailedPreparedRootPinsSharedHandleAfterRetirement) {
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  ASSERT_TRUE(handle->SetOwnerShutdownTimeout(1'000'000'000).ok());
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak = handle;
  failed = View("failed-prepared");
  failed->ros2_control_runtime_handle = handle;
  retired = View("retired");
  retired->ros2_control_runtime_handle = handle;

  Release();
  EXPECT_FALSE(retired);
  EXPECT_EQ(reclaimed, 1);
  EXPECT_FALSE(RetainedRos2ControlRuntimeHandleTestAccess::Closing(*handle));
  handle.reset();
  EXPECT_FALSE(weak.expired());

  failed.reset();
  EXPECT_TRUE(weak.expired());
  Release();
  EXPECT_EQ(reclaimed, 2);
}

TEST_F(RetirementFixture, UniqueHandleQuiescesBeforeRetiredSlotReclaimed) {
  auto handle = RetainedRos2ControlRuntimeHandle::Create();
  ASSERT_TRUE(handle->SetOwnerShutdownTimeout(1'000'000'000).ok());
  retired = View("retired");
  retired->ros2_control_runtime_handle = handle;
  Release();
  // An owner-less handle has nothing to join: quiesce closes it and settles
  // without setting the all-owners-joined flag.
  EXPECT_TRUE(RetainedRos2ControlRuntimeHandleTestAccess::Closing(*handle));
  EXPECT_FALSE(retired);
  EXPECT_EQ(reclaimed, 1);
}


}  // namespace
}  // namespace host_detail
}  // namespace manyforge::roboplan_backend
