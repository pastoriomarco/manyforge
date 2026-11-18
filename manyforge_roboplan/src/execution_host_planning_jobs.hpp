#pragma once

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {

/// Pick/place planning jobs held at once: accepted, planning, or finished and
/// not yet released. The same bound as the submission-ordered lane.
inline constexpr std::size_t kMaximumPlanningJobs = 8U;

enum class PlanningJobState {
  /// Accepted and waiting on the submission-ordered lane.
  kAccepted,
  kPlanning,
  /// Planning returned a result (`planned` or `no_feasible_candidate`).
  kDone,
  /// Planning returned a typed error.
  kFailed,
  /// Its own cancel arrived before it had a result.
  kCancelled,
};

[[nodiscard]] std::string_view PlanningJobStateName(PlanningJobState state) noexcept;

/// What a read of one job returns.
struct PlanningJobSnapshot final {
  std::string job_id;
  bool pick{};
  PlanningJobState state{PlanningJobState::kAccepted};
  /// kDone: the `planning_result` object; kFailed: the `error` object.
  std::shared_ptr<const content_identity::CanonicalValue> outcome;
};

/// The wire form of one job (`manyforge.planning_job.v1`).
[[nodiscard]] content_identity::CanonicalValue
PlanningJobValue(const PlanningJobSnapshot &job);

/// One job as the event stream's `state.planning_jobs` lists it: never the
/// result, which is read once with `read_planning_job`.
struct PlanningJobStreamRow final {
  std::string job_id;
  bool pick{};
  PlanningJobState state{PlanningJobState::kAccepted};
  std::optional<core::ProgramRunNumber> program_run_id;
};

/// `state.planning_jobs`: at most kMaximumPlanningJobs rows, in acceptance
/// order, each {job_id, kind, program_run_id|null, state}.
[[nodiscard]] content_identity::CanonicalValue
PlanningJobsStreamValue(const std::vector<PlanningJobStreamRow> &rows);

/// The host's pick/place planning jobs (OD-213 (1)). Memory only; its mutex is
/// a leaf: no method calls out while holding it. A job is named on the wire by
/// its `sequence_id` and on the lane by a ticket, so a lane entry never runs a
/// later job that reuses the name.
class PlanningJobTable final {
public:
  struct Accepted final {
    std::uint64_t ticket{};
    PlanningJobSnapshot job;
  };

  /// Admits a job in state accepted. Refused when the name is held by a job
  /// not yet released (`planning_job_exists`) or the table is full
  /// (`execution_planning_jobs_busy`).
  [[nodiscard]] core::Result<Accepted>
  Accept(std::string job_id, bool pick,
         std::optional<core::ProgramRunNumber> program_run_id);

  /// Drops a job whose lane entry could not be queued.
  void Withdraw(std::uint64_t ticket) noexcept;

  /// The lane takes the job: it becomes planning and planning consults the
  /// returned source. Empty when the job was cancelled or released while it
  /// waited; the lane then skips it.
  [[nodiscard]] std::optional<std::stop_source> Begin(std::uint64_t ticket) noexcept;

  /// Records what planning returned: `planning_result` when `succeeded`, else
  /// the `error`. A job whose own cancel arrived and that has no result
  /// becomes cancelled. A released job is dropped.
  void Finish(std::uint64_t ticket, bool succeeded,
              content_identity::CanonicalValue outcome);

  [[nodiscard]] core::Result<PlanningJobSnapshot> Read(std::string_view job_id) const;

  /// Cancels this job only. Waiting: cancelled at once. Planning: its stop
  /// source is set and it becomes cancelled when planning returns. Finished:
  /// unchanged.
  [[nodiscard]] core::Result<PlanningJobSnapshot> Cancel(std::string_view job_id);

  /// Drops a finished job (`planning_job_unfinished` while it waits or plans).
  [[nodiscard]] core::Result<void> Release(std::string_view job_id);

  /// Sets the stop source of every unfinished job, whose planning then fails
  /// with the planner's own cancellation code. With `ended_run`, that run's
  /// jobs are also released (an unfinished one when planning returns).
  void Preempt(std::optional<core::ProgramRunNumber> ended_run) noexcept;

  /// Every held job (not the released ones), in acceptance order. Each
  /// change above raises the event stream's change signal after the unlock.
  [[nodiscard]] std::vector<PlanningJobStreamRow> StreamRows() const;

private:
  struct Job final {
    std::uint64_t ticket{};
    std::optional<core::ProgramRunNumber> program_run_id;
    bool cancel_requested{};
    bool released{};
    std::stop_source stop;
    PlanningJobSnapshot snapshot;
  };

  [[nodiscard]] Job *FindLocked(std::string_view job_id);
  [[nodiscard]] const Job *FindLocked(std::string_view job_id) const;

  mutable std::mutex mutex_;
  std::uint64_t next_ticket_{1U};
  std::vector<Job> jobs_;
};

} // namespace manyforge::roboplan_backend::host_detail
