// Compile-time test: verify all contract headers compile together
// and are self-contained (each header includes its own dependencies).
#include <gtest/gtest.h>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/movement_config.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/scene_snapshot_dto.hpp>
#include <manyforge_core/types/collision_event.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iscene_provider.hpp>
#include <manyforge_core/ports/icontroller.hpp>
#include <manyforge_core/ports/icollision_monitor.hpp>

TEST(ContractsCompile, ResultOk) {
  auto r = manyforge::core::Result<int>::Ok(42);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.value(), 42);
}

TEST(ContractsCompile, ResultErr) {
  auto r = manyforge::core::Result<int>::Err(manyforge::core::ErrorCode::kPlanningFailed);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.error(), manyforge::core::ErrorCode::kPlanningFailed);
}

TEST(ContractsCompile, ResultVoid) {
  auto ok = manyforge::core::Result<void>::Ok();
  EXPECT_TRUE(ok.ok());

  auto err = manyforge::core::Result<void>::Err(manyforge::core::ErrorCode::kInternalError);
  EXPECT_FALSE(err.ok());
  EXPECT_EQ(err.error(), manyforge::core::ErrorCode::kInternalError);
}

TEST(ContractsCompile, DefaultDTOs) {
  manyforge::core::MovementConfig config;
  EXPECT_GT(config.planning_time, 0.0);

  manyforge::core::TrajectoryDTO traj;
  EXPECT_TRUE(traj.empty());

  manyforge::core::SceneSnapshotDTO scene;
  EXPECT_EQ(scene.version, 0u);

  manyforge::core::CollisionEvent event;
  EXPECT_EQ(event.severity, manyforge::core::CollisionSeverity::kHardStop);

  manyforge::core::ExecutionStatus status;
  EXPECT_EQ(status.state, manyforge::core::ExecutionState::kIdle);
}

TEST(ContractsCompile, EnumToString) {
  EXPECT_STREQ(manyforge::core::to_string(manyforge::core::ErrorCode::kSuccess), "Success");
  EXPECT_STREQ(manyforge::core::to_string(manyforge::core::ExecutionState::kExecuting),
               "Executing");
}
