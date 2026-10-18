#include <manyforge_core/execution_correlation_tracker.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>

#include <gtest/gtest.h>

namespace manyforge::core {
namespace {

ExecuteRequestDTO make_request() {
  auto request = testing::make_test_execute_request(
      {"joint1", "joint2"}, "run-1");
  request.move_id = "move-1";
  ActionSpanDTO span;
  span.action_source = ActionSource::kProgramNode;
  span.action_span_id = "span-1";
  span.program_run_id = "span-run-1";
  span.node_instance_id = "node-1";
  span.node_attempt_id = "attempt-1";
  span.parent_node_attempt_id = "parent-1";
  request.action_span = span;
  return request;
}

TEST(ExecutionCorrelationTrackerTest, StampsDetailsAndSnapshot) {
  ExecutionCorrelationTracker tracker;
  tracker.set_from_request(make_request());

  std::map<std::string, std::string> details;
  tracker.stamp_details(details);

  EXPECT_EQ(tracker.move_id(), "move-1");
  EXPECT_EQ(details.at("program_run_id"), "run-1");
  EXPECT_EQ(details.at("action_span_id"), "span-1");
  EXPECT_EQ(details.at("action_span_program_run_id"), "span-run-1");
  EXPECT_EQ(details.at("node_instance_id"), "node-1");
  EXPECT_EQ(details.at("node_attempt_id"), "attempt-1");
  EXPECT_EQ(details.at("parent_node_attempt_id"), "parent-1");

  ExecutionSnapshot snapshot;
  tracker.stamp_snapshot(snapshot);
  ASSERT_TRUE(snapshot.action_span.has_value());
  EXPECT_EQ(snapshot.action_span->action_span_id, "span-1");
}

TEST(ExecutionCorrelationTrackerTest, PreparedHandoverSwapsAllCorrelationWithoutLosingOldIdentity) {
  ExecutionCorrelationTracker current;
  ExecutionCorrelationTracker candidate;
  const auto old_request = make_request();
  auto new_request = make_request();
  new_request.move_id = "move-2";
  new_request.binding.program_run_id = ProgramRunId("run-2");
  new_request.binding.operation_id = OperationId("operation-2");
  new_request.action_span->node_attempt_id = "attempt-2";
  current.set_from_request(old_request);
  candidate.set_from_request(new_request);
  EXPECT_EQ(current.move_id(), old_request.move_id);
  current.swap_prepared(candidate);
  EXPECT_EQ(current.move_id(), new_request.move_id);
  EXPECT_EQ(current.resolved_binding(), new_request.binding);
  EXPECT_EQ(current.action_span(), new_request.action_span);
  EXPECT_EQ(candidate.move_id(), old_request.move_id);
  EXPECT_EQ(candidate.resolved_binding(), old_request.binding);
  EXPECT_EQ(candidate.action_span(), old_request.action_span);
  std::map<std::string, std::string> details;
  current.stamp_details(details);
  EXPECT_EQ(details.at("program_run_id"), "run-2");
  EXPECT_EQ(details.at("node_attempt_id"), "attempt-2");
}

TEST(ExecutionCorrelationTrackerTest, ClearsMoveSeparatelyFromAuditSpan) {
  ExecutionCorrelationTracker tracker;
  tracker.set_from_request(make_request());

  tracker.clear_move_id();
  EXPECT_TRUE(tracker.move_id().empty());

  std::map<std::string, std::string> details;
  tracker.stamp_details(details);
  EXPECT_EQ(details.at("program_run_id"), "run-1");
  EXPECT_EQ(details.at("action_span_id"), "span-1");

  tracker.clear_program_run_and_action_span();
  details.clear();
  tracker.stamp_details(details);
  EXPECT_TRUE(details.empty());
}

}  // namespace
}  // namespace manyforge::core
