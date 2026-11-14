// Pick/place planning jobs (OD-213 (1)): the host's table of at most eight
// jobs and the three memory-only commands that read, cancel and release one.
#include "execution_host_commands.hpp"
#include "execution_host_planning_jobs.hpp"

#include <gtest/gtest.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/execution_event_change_signal.hpp>

#include <exception>
#include <optional>
#include <stop_token>
#include <string>

namespace manyforge::roboplan_backend::host_detail {
namespace {

using Value = content_identity::CanonicalValue;
using State = PlanningJobState;

const core::ProgramRunNumber kRun{7U};
const core::ProgramRunNumber kOtherRun{8U};

Value PlanResult(const char *status) {
  return Value(Value::Object{{"status", Value(status)}});
}

Value PlanError(const char *code) {
  return Value(Value::Object{{"code", Value(code)}});
}

std::uint64_t AcceptJob(PlanningJobTable &table, const std::string &id,
                        std::optional<core::ProgramRunNumber> run = kRun) {
  auto accepted = table.Accept(id, true, run);
  EXPECT_TRUE(accepted.ok());
  return accepted.ok() ? accepted.value().ticket : 0U;
}

State StateOf(const PlanningJobTable &table, const std::string &id) {
  const auto job = table.Read(id);
  EXPECT_TRUE(job.ok()) << id;
  return job.ok() ? job.value().state : State::kFailed;
}

TEST(PlanningJobTable, AcceptsAtOnceWhileAnEarlierJobPlans) {
  PlanningJobTable table;
  const auto first = AcceptJob(table, "a");
  const auto source = table.Begin(first);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(StateOf(table, "a"), State::kPlanning);
  // The second request is answered while the first still plans.
  auto second = table.Accept("b", false, kRun);
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(second.value().job.state, State::kAccepted);
  EXPECT_FALSE(second.value().job.pick);
  EXPECT_NE(second.value().ticket, first);
  EXPECT_EQ(StateOf(table, "a"), State::kPlanning);
  EXPECT_EQ(StateOf(table, "b"), State::kAccepted);
}

TEST(PlanningJobTable, ReadReturnsEachStateAndTheResultTheNodeConsumes) {
  PlanningJobTable table;
  const auto done = AcceptJob(table, "done");
  const auto failed = AcceptJob(table, "failed");
  EXPECT_EQ(StateOf(table, "done"), State::kAccepted);
  ASSERT_TRUE(table.Begin(done).has_value());
  ASSERT_TRUE(table.Begin(failed).has_value());
  EXPECT_EQ(StateOf(table, "done"), State::kPlanning);
  table.Finish(done, true, PlanResult("planned"));
  table.Finish(failed, false, PlanError("pick_planning_cancelled"));

  const auto read_done = table.Read("done");
  ASSERT_TRUE(read_done.ok());
  EXPECT_EQ(read_done.value().state, State::kDone);
  ASSERT_TRUE(read_done.value().outcome);
  EXPECT_EQ(*read_done.value().outcome, PlanResult("planned"));
  const auto done_value = PlanningJobValue(read_done.value());
  EXPECT_EQ(*done_value.find_object_member("state"), Value("done"));
  EXPECT_EQ(*done_value.find_object_member("kind"), Value("pick"));
  EXPECT_EQ(*done_value.find_object_member("job_id"), Value("done"));
  EXPECT_EQ(*done_value.find_object_member("planning_result"), PlanResult("planned"));
  EXPECT_TRUE(done_value.find_object_member("error")->is_null());
  EXPECT_EQ(*done_value.find_object_member("schema_version"),
            Value("manyforge.planning_job.v1"));

  const auto read_failed = table.Read("failed");
  ASSERT_TRUE(read_failed.ok());
  EXPECT_EQ(read_failed.value().state, State::kFailed);
  const auto failed_value = PlanningJobValue(read_failed.value());
  EXPECT_EQ(*failed_value.find_object_member("error"),
            PlanError("pick_planning_cancelled"));
  EXPECT_TRUE(failed_value.find_object_member("planning_result")->is_null());

  const auto unknown = table.Read("never");
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, "planning_job_unknown");
}

TEST(PlanningJobTable, CancelStopsThisJobOnlyWhileAnotherContinues) {
  PlanningJobTable table;
  const auto mine = AcceptJob(table, "mine");
  const auto other = AcceptJob(table, "other", kOtherRun);
  const auto mine_source = table.Begin(mine);
  const auto other_source = table.Begin(other);
  ASSERT_TRUE(mine_source && other_source);

  const auto cancelled = table.Cancel("mine");
  ASSERT_TRUE(cancelled.ok());
  EXPECT_TRUE(mine_source->stop_requested());
  EXPECT_FALSE(other_source->stop_requested());
  // It plans until planning returns, then it is cancelled, not failed.
  EXPECT_EQ(cancelled.value().state, State::kPlanning);
  table.Finish(mine, false, PlanError("pick_planning_cancelled"));
  EXPECT_EQ(StateOf(table, "mine"), State::kCancelled);
  // A cancelled job carries neither a result nor an error.
  const auto mine_value = PlanningJobValue(table.Read("mine").value());
  EXPECT_TRUE(mine_value.find_object_member("error")->is_null());
  EXPECT_TRUE(mine_value.find_object_member("planning_result")->is_null());

  EXPECT_EQ(StateOf(table, "other"), State::kPlanning);
  table.Finish(other, true, PlanResult("planned"));
  EXPECT_EQ(StateOf(table, "other"), State::kDone);
}

TEST(PlanningJobTable, CancelWhileWaitingSkipsThePlanning) {
  PlanningJobTable table;
  const auto ticket = AcceptJob(table, "queued");
  const auto cancelled = table.Cancel("queued");
  ASSERT_TRUE(cancelled.ok());
  EXPECT_EQ(cancelled.value().state, State::kCancelled);
  // The lane reaches its entry later and does not plan.
  EXPECT_FALSE(table.Begin(ticket).has_value());
  EXPECT_EQ(StateOf(table, "queued"), State::kCancelled);
}

TEST(PlanningJobTable, AHaltInEachStateCancelsOnlyWhatIsUnfinished) {
  PlanningJobTable table;
  const auto accepted = AcceptJob(table, "accepted");
  const auto planning = AcceptJob(table, "planning");
  const auto done = AcceptJob(table, "done");
  const auto failed = AcceptJob(table, "failed");
  const auto cancelled = AcceptJob(table, "cancelled");
  const auto planning_source = table.Begin(planning);
  ASSERT_TRUE(planning_source && table.Begin(done) && table.Begin(failed));
  table.Finish(done, true, PlanResult("no_feasible_candidate"));
  table.Finish(failed, false, PlanError("motion_preview_projection_unavailable"));
  ASSERT_TRUE(table.Cancel("cancelled").ok());

  // The node's halt is its own cancel, in whatever state the job is.
  EXPECT_EQ(table.Cancel("accepted").value().state, State::kCancelled);
  EXPECT_FALSE(table.Begin(accepted).has_value());
  EXPECT_EQ(table.Cancel("planning").value().state, State::kPlanning);
  EXPECT_TRUE(planning_source->stop_requested());
  table.Finish(planning, false, PlanError("pick_planning_cancelled"));
  EXPECT_EQ(StateOf(table, "planning"), State::kCancelled);
  // A finished job keeps its outcome.
  const auto kept_done = table.Cancel("done");
  EXPECT_EQ(kept_done.value().state, State::kDone);
  EXPECT_EQ(*kept_done.value().outcome, PlanResult("no_feasible_candidate"));
  const auto kept_failed = table.Cancel("failed");
  EXPECT_EQ(kept_failed.value().state, State::kFailed);
  EXPECT_EQ(*kept_failed.value().outcome,
            PlanError("motion_preview_projection_unavailable"));
  EXPECT_EQ(table.Cancel("cancelled").value().state, State::kCancelled);
  static_cast<void>(cancelled);
  EXPECT_EQ(table.Cancel("never").error().code, "planning_job_unknown");
}

TEST(PlanningJobTable, AResultThatArrivesAfterItsCancelIsKept) {
  PlanningJobTable table;
  const auto ticket = AcceptJob(table, "late");
  ASSERT_TRUE(table.Begin(ticket).has_value());
  ASSERT_TRUE(table.Cancel("late").ok());
  table.Finish(ticket, true, PlanResult("planned"));
  EXPECT_EQ(StateOf(table, "late"), State::kDone);
}

TEST(PlanningJobTable, HoldsAtMostEightJobsAndRefusesAHeldName) {
  PlanningJobTable table;
  for (std::size_t index = 0U; index < kMaximumPlanningJobs; ++index)
    AcceptJob(table, "job" + std::to_string(index));
  const auto full = table.Accept("one-more", true, kRun);
  ASSERT_FALSE(full.ok());
  EXPECT_EQ(full.error().code, "execution_planning_jobs_busy");
  const auto again = table.Accept("job0", true, kRun);
  ASSERT_FALSE(again.ok());
  // The name is checked first: a held job is never displaced.
  EXPECT_EQ(again.error().code, "planning_job_exists");
}

TEST(PlanningJobTable, ReleaseDropsOnlyAFinishedJob) {
  PlanningJobTable table;
  const auto ticket = AcceptJob(table, "job");
  auto unfinished = table.Release("job");
  ASSERT_FALSE(unfinished.ok());
  EXPECT_EQ(unfinished.error().code, "planning_job_unfinished");
  EXPECT_EQ(unfinished.error().details.at("state"), "accepted");
  ASSERT_TRUE(table.Begin(ticket).has_value());
  EXPECT_EQ(table.Release("job").error().details.at("state"), "planning");
  table.Finish(ticket, true, PlanResult("planned"));
  ASSERT_TRUE(table.Release("job").ok());
  EXPECT_EQ(table.Read("job").error().code, "planning_job_unknown");
  EXPECT_EQ(table.Release("job").error().code, "planning_job_unknown");
  // The released name may be used again, and the slot is free.
  for (std::size_t index = 0U; index < kMaximumPlanningJobs; ++index)
    AcceptJob(table, "job" + std::to_string(index));
}

TEST(PlanningJobTable, AJobReleasedWhileQueuedIsNeverPlanned) {
  PlanningJobTable table;
  const auto old_ticket = AcceptJob(table, "name");
  ASSERT_TRUE(table.Cancel("name").ok());
  ASSERT_TRUE(table.Release("name").ok());
  // A new job reuses the name; the old lane entry does not run it.
  const auto new_ticket = AcceptJob(table, "name");
  EXPECT_FALSE(table.Begin(old_ticket).has_value());
  EXPECT_EQ(StateOf(table, "name"), State::kAccepted);
  EXPECT_TRUE(table.Begin(new_ticket).has_value());
}

TEST(PlanningJobTable, RunEndReleasesThatRunsJobsAndPreemptsTheRest) {
  PlanningJobTable table;
  const auto run_done = AcceptJob(table, "run-done");
  const auto run_planning = AcceptJob(table, "run-planning");
  const auto run_waiting = AcceptJob(table, "run-waiting");
  const auto other = AcceptJob(table, "other", kOtherRun);
  const auto manual = AcceptJob(table, "manual", std::nullopt);
  ASSERT_TRUE(table.Begin(run_done).has_value());
  table.Finish(run_done, true, PlanResult("planned"));
  const auto planning_source = table.Begin(run_planning);
  const auto other_source = table.Begin(other);
  ASSERT_TRUE(planning_source && other_source);

  table.Preempt(kRun);
  // The ended run's jobs are gone at once, finished or not.
  for (const auto *id : {"run-done", "run-planning", "run-waiting"})
    EXPECT_EQ(table.Read(id).error().code, "planning_job_unknown") << id;
  EXPECT_TRUE(planning_source->stop_requested());
  EXPECT_FALSE(table.Begin(run_waiting).has_value());
  table.Finish(run_planning, false, PlanError("pick_planning_cancelled"));
  EXPECT_EQ(table.Read("run-planning").error().code, "planning_job_unknown");
  // Other jobs are preempted, not released: their planning fails with the
  // planner's own code.
  EXPECT_TRUE(other_source->stop_requested());
  table.Finish(other, false, PlanError("pick_planning_cancelled"));
  EXPECT_EQ(StateOf(table, "other"), State::kFailed);
  EXPECT_EQ(StateOf(table, "manual"), State::kAccepted);
  // A preempted waiting job reaches the planner already stopped.
  const auto manual_source = table.Begin(manual);
  ASSERT_TRUE(manual_source.has_value());
  EXPECT_TRUE(manual_source->stop_requested());
  // The ended run's slots are free again.
  for (const auto *id : {"x", "y", "z", "w", "v", "u"}) AcceptJob(table, id);
  EXPECT_EQ(table.Accept("full", true, kRun).error().code,
            "execution_planning_jobs_busy");
}

TEST(PlanningJobTable, PreemptWithoutARunReleasesNothing) {
  PlanningJobTable table;
  const auto ticket = AcceptJob(table, "job");
  ASSERT_TRUE(table.Begin(ticket).has_value());
  table.Finish(ticket, true, PlanResult("planned"));
  table.Preempt(std::nullopt);
  EXPECT_EQ(StateOf(table, "job"), State::kDone);
}

TEST(PlanningJobTable, WithdrawDropsAJobTheLaneCouldNotTake) {
  PlanningJobTable table;
  const auto ticket = AcceptJob(table, "job");
  table.Withdraw(ticket);
  EXPECT_EQ(table.Read("job").error().code, "planning_job_unknown");
  EXPECT_FALSE(table.Begin(ticket).has_value());
}

TEST(PlanningJobStream, ListsHeldJobsInAcceptanceOrderWithoutResults) {
  PlanningJobTable table;
  const auto first = AcceptJob(table, "first");
  AcceptJob(table, "manual", std::nullopt);
  const auto released = AcceptJob(table, "released", kOtherRun);
  ASSERT_TRUE(table.Begin(first).has_value());
  table.Finish(first, true, PlanResult("planned"));
  ASSERT_TRUE(table.Begin(released).has_value());
  table.Preempt(kOtherRun);  // released, still planning: not listed

  const auto value = PlanningJobsStreamValue(table.StreamRows());
  const auto expected = Value(Value::Array{
      Value(Value::Object{{"job_id", Value("first")},
                          {"kind", Value("pick")},
                          {"program_run_id", Value(7.0)},
                          {"state", Value("done")}}),
      Value(Value::Object{{"job_id", Value("manual")},
                          {"kind", Value("pick")},
                          {"program_run_id", Value(nullptr)},
                          {"state", Value("accepted")}}),
  });
  EXPECT_EQ(value, expected);
  EXPECT_EQ(PlanningJobsStreamValue({}), Value(Value::Array{}));
}

TEST(PlanningJobStream, EveryChangeRaisesTheStreamSignal) {
  auto &signal = core::execution_event_change_signal();
  PlanningJobTable table;
  auto before = signal.Generation();
  const auto ticket = AcceptJob(table, "job");
  EXPECT_NE(signal.Generation(), before);
  before = signal.Generation();
  ASSERT_TRUE(table.Begin(ticket).has_value());
  EXPECT_NE(signal.Generation(), before);
  before = signal.Generation();
  table.Finish(ticket, true, PlanResult("planned"));
  EXPECT_NE(signal.Generation(), before);
  before = signal.Generation();
  ASSERT_TRUE(table.Release("job").ok());
  EXPECT_NE(signal.Generation(), before);
}

Command DecodeJobCommand(const char *name, Value job_id) {
  return DecodeCommand(content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value(name)},
      {"job_id", std::move(job_id)}})));
}

TEST(PlanningJobCommandCodec, DecodesTheThreeMemoryOnlyCommands) {
  const auto id = std::string(32U, 'a');
  const auto read = DecodeJobCommand("read_planning_job", Value(id));
  EXPECT_EQ(read.kind, CommandKind::kReadPlanningJob);
  EXPECT_EQ(read.planning_job_id, std::optional<std::string>(id));
  EXPECT_EQ(DecodeJobCommand("cancel_planning_job", Value(id)).kind,
            CommandKind::kCancelPlanningJob);
  EXPECT_EQ(DecodeJobCommand("release_planning_job", Value(id)).kind,
            CommandKind::kReleasePlanningJob);
  for (const auto kind : {CommandKind::kReadPlanningJob,
                          CommandKind::kCancelPlanningJob,
                          CommandKind::kReleasePlanningJob}) {
    // Memory only, on the control socket: never on a deferred lane, never a
    // planning preemption.
    EXPECT_TRUE(IsPlanningJobControl(kind));
    EXPECT_FALSE(IsSubmissionOrdered(kind));
    EXPECT_FALSE(ReadsManagerScene(kind));
    EXPECT_FALSE(IsPlanningJobCommand(kind));
    EXPECT_FALSE(PreemptsPlanning(kind));
    EXPECT_FALSE(IsStopLaneRead(kind));
    EXPECT_FALSE(IsComponentLaneCommand(kind));
  }
}

TEST(PlanningJobCommandCodec, RefusesMalformedRequests) {
  EXPECT_THROW(DecodeJobCommand("read_planning_job", Value("")), std::exception);
  EXPECT_THROW(DecodeJobCommand("read_planning_job",
                                Value(std::string(core::kMaxPickPlaceSequenceIdBytes + 1U, 'a'))),
               std::exception);
  EXPECT_THROW(DecodeJobCommand("cancel_planning_job", Value(7.0)), std::exception);
  EXPECT_THROW(DecodeCommand(content_identity::canonical_json(Value(Value::Object{
                   {"api_version", Value(std::string(kCommandApiVersion))},
                   {"command", Value("release_planning_job")},
                   {"extra", Value(true)},
                   {"job_id", Value("a")}}))),
               std::exception);
}

TEST(PlanningJobCommandCodec, PickAndPlacePlanningStayOnTheSubmissionLane) {
  for (const auto kind : {CommandKind::kPlanPick, CommandKind::kPlanPlace}) {
    EXPECT_TRUE(IsPlanningJobCommand(kind));
    EXPECT_FALSE(IsPlanningJobControl(kind));
    EXPECT_TRUE(IsSubmissionOrdered(kind));
  }
  EXPECT_FALSE(IsPlanningJobCommand(CommandKind::kPreviewMotion));
  EXPECT_FALSE(IsPlanningJobCommand(CommandKind::kSubmit));
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
