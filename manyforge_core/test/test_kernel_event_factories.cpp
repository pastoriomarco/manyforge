#include <gtest/gtest.h>

#include <manyforge_core/types/kernel_event_factories.hpp>

namespace manyforge::core {
namespace {

TEST(KernelEventFactoriesTest, GenericEventDefaultsStateAndError) {
  auto event = make_kernel_event(KernelEventType::kPlanStarted, 42, 123,
                                 {{"move_id", "pick"}});

  EXPECT_EQ(event.type, KernelEventType::kPlanStarted);
  EXPECT_EQ(event.execution_id, 42u);
  EXPECT_EQ(event.timestamp_ns, 123);
  EXPECT_EQ(event.from_state, ExecutionState::kIdle);
  EXPECT_EQ(event.to_state, ExecutionState::kIdle);
  EXPECT_FALSE(event.error.has_value());
  EXPECT_EQ(event.details.at("move_id"), "pick");
}

TEST(KernelEventFactoriesTest, StateTransitionSetsOnlyStateFields) {
  auto event = make_state_transition_event(
      7, 99, ExecutionState::kPlanning, ExecutionState::kExecuting);

  EXPECT_EQ(event.type, KernelEventType::kStateTransition);
  EXPECT_EQ(event.execution_id, 7u);
  EXPECT_EQ(event.timestamp_ns, 99);
  EXPECT_EQ(event.from_state, ExecutionState::kPlanning);
  EXPECT_EQ(event.to_state, ExecutionState::kExecuting);
  EXPECT_FALSE(event.error.has_value());
  EXPECT_TRUE(event.details.empty());
}

TEST(KernelEventFactoriesTest, ErrorRaisedStoresErrorAndDetails) {
  auto event = make_error_raised_event(
      8, 100, ErrorDTO{kErrorStopFailed, "stop failed", {{"phase", "stop"}}},
      {{"source", "test"}});

  EXPECT_EQ(event.type, KernelEventType::kErrorRaised);
  EXPECT_EQ(event.execution_id, 8u);
  EXPECT_EQ(event.timestamp_ns, 100);
  EXPECT_EQ(event.from_state, ExecutionState::kIdle);
  EXPECT_EQ(event.to_state, ExecutionState::kIdle);
  ASSERT_TRUE(event.error.has_value());
  EXPECT_EQ(event.error->code, kErrorStopFailed);
  EXPECT_EQ(event.error->message, "stop failed");
  EXPECT_EQ(event.error->details.at("phase"), "stop");
  EXPECT_EQ(event.details.at("source"), "test");
}

}  // namespace
}  // namespace manyforge::core
