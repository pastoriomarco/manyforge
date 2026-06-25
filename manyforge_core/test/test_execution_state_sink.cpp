#include <manyforge_core/ports/iexecution_state_sink.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using manyforge::core::ExecutionStateSinkResult;
using manyforge::core::ExecutionStateSinkStatus;
using manyforge::core::ExecutionStateUpdateMode;
using manyforge::core::IExecutionStateSink;
using manyforge::core::execution_state_sink_status_reason;

static_assert(std::is_trivially_copyable_v<ExecutionStateSinkResult>);
static_assert(sizeof(ExecutionStateSinkResult) == sizeof(ExecutionStateSinkStatus));
static_assert(noexcept(std::declval<ExecutionStateSinkResult>().applied()));
static_assert(noexcept(execution_state_sink_status_reason(
    ExecutionStateSinkStatus::kApplied)));
static_assert(std::is_same_v<decltype(execution_state_sink_status_reason(
                                 ExecutionStateSinkStatus::kApplied)),
                             std::string_view>);
static_assert(std::has_virtual_destructor_v<IExecutionStateSink>);
static_assert(std::is_abstract_v<IExecutionStateSink>);

class FakeExecutionStateSink final : public IExecutionStateSink {
 public:
  ExecutionStateSinkStatus next_positions_status = ExecutionStateSinkStatus::kApplied;
  ExecutionStateSinkStatus next_joint_status = ExecutionStateSinkStatus::kApplied;
  std::optional<double> next_joint_read = std::nullopt;

  std::string last_group_id;
  std::string last_joint_name;
  std::vector<double> last_positions;
  double last_joint_position = 0.0;
  std::optional<ExecutionStateUpdateMode> last_positions_mode;
  std::optional<ExecutionStateUpdateMode> last_joint_mode;

  ExecutionStateSinkResult apply_group_joint_positions(
      const std::string& group_id,
      const std::vector<double>& positions_rad,
      ExecutionStateUpdateMode mode) override {
    last_group_id = group_id;
    last_positions = positions_rad;
    last_positions_mode = mode;
    return ExecutionStateSinkResult{next_positions_status};
  }

  std::optional<double> group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const override {
    (void)group_id;
    (void)joint_name;
    return next_joint_read;
  }

  ExecutionStateSinkResult apply_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name,
      double position_rad,
      ExecutionStateUpdateMode mode) override {
    last_group_id = group_id;
    last_joint_name = joint_name;
    last_joint_position = position_rad;
    last_joint_mode = mode;
    return ExecutionStateSinkResult{next_joint_status};
  }
};

TEST(ExecutionStateSinkResult, ReportsAppliedFromEnumOnlyStatus) {
  EXPECT_TRUE(ExecutionStateSinkResult{}.applied());
  EXPECT_TRUE(ExecutionStateSinkResult{ExecutionStateSinkStatus::kApplied}.applied());
  EXPECT_FALSE(ExecutionStateSinkResult{ExecutionStateSinkStatus::kBusy}.applied());
  EXPECT_FALSE(
      ExecutionStateSinkResult{ExecutionStateSinkStatus::kException}.applied());
}

TEST(ExecutionStateSinkStatusReason, ProvidesStableStaticTokens) {
  EXPECT_EQ(execution_state_sink_status_reason(ExecutionStateSinkStatus::kApplied),
            "applied");
  EXPECT_EQ(execution_state_sink_status_reason(ExecutionStateSinkStatus::kBusy),
            "scene_sink_busy");
  EXPECT_EQ(
      execution_state_sink_status_reason(ExecutionStateSinkStatus::kInvalidGroup),
      "invalid_group");
  EXPECT_EQ(
      execution_state_sink_status_reason(ExecutionStateSinkStatus::kInvalidJoint),
      "invalid_joint");
  EXPECT_EQ(execution_state_sink_status_reason(
                ExecutionStateSinkStatus::kInvalidPositions),
            "invalid_positions");
  EXPECT_EQ(execution_state_sink_status_reason(
                ExecutionStateSinkStatus::kStateUnavailable),
            "scene_state_unavailable");
  EXPECT_EQ(
      execution_state_sink_status_reason(ExecutionStateSinkStatus::kException),
      "scene_sink_exception");
  EXPECT_EQ(execution_state_sink_status_reason(
                static_cast<ExecutionStateSinkStatus>(999)),
            "unknown_execution_state_sink_status");
}

TEST(FakeExecutionStateSink, PropagatesGroupPositionModeAndStatus) {
  FakeExecutionStateSink sink;
  sink.next_positions_status = ExecutionStateSinkStatus::kBusy;

  const auto result = sink.apply_group_joint_positions(
      "arm", {0.1, 0.2, 0.3}, ExecutionStateUpdateMode::kBestEffort);

  EXPECT_EQ(result.status, ExecutionStateSinkStatus::kBusy);
  EXPECT_FALSE(result.applied());
  EXPECT_EQ(sink.last_group_id, "arm");
  EXPECT_EQ(sink.last_positions, (std::vector<double>{0.1, 0.2, 0.3}));
  ASSERT_TRUE(sink.last_positions_mode.has_value());
  EXPECT_EQ(*sink.last_positions_mode, ExecutionStateUpdateMode::kBestEffort);
}

TEST(FakeExecutionStateSink, PropagatesSingleJointModeStatusAndRead) {
  FakeExecutionStateSink sink;
  sink.next_joint_read = 0.4;
  sink.next_joint_status = ExecutionStateSinkStatus::kInvalidJoint;

  EXPECT_EQ(sink.group_joint_position("gripper", "finger_joint"), 0.4);

  const auto result = sink.apply_group_joint_position(
      "gripper", "finger_joint", 0.7, ExecutionStateUpdateMode::kBlocking);

  EXPECT_EQ(result.status, ExecutionStateSinkStatus::kInvalidJoint);
  EXPECT_FALSE(result.applied());
  EXPECT_EQ(sink.last_group_id, "gripper");
  EXPECT_EQ(sink.last_joint_name, "finger_joint");
  EXPECT_DOUBLE_EQ(sink.last_joint_position, 0.7);
  ASSERT_TRUE(sink.last_joint_mode.has_value());
  EXPECT_EQ(*sink.last_joint_mode, ExecutionStateUpdateMode::kBlocking);
}

}  // namespace
