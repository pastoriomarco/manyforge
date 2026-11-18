#include "execution_host_planning_jobs.hpp"

#include <manyforge_core/execution_event_change_signal.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

using content_identity::CanonicalValue;

namespace {

[[nodiscard]] bool Finished(const PlanningJobState state) noexcept {
  return state == PlanningJobState::kDone ||
         state == PlanningJobState::kFailed ||
         state == PlanningJobState::kCancelled;
}

/// Declared before a method's lock: the event stream's change signal is
/// raised after the unlock, so a stream reader sees the new state.
struct NotifyStreamAfterUnlock final {
  ~NotifyStreamAfterUnlock() { core::execution_event_change_signal().Notify(); }
};

[[nodiscard]] core::ErrorDTO UnknownJob(const std::string_view job_id) {
  return core::ErrorDTO{"planning_job_unknown",
                        "no planning job of this name is held",
                        {{"job_id", std::string(job_id)}}};
}

} // namespace

std::string_view PlanningJobStateName(const PlanningJobState state) noexcept {
  switch (state) {
  case PlanningJobState::kAccepted:
    return "accepted";
  case PlanningJobState::kPlanning:
    return "planning";
  case PlanningJobState::kDone:
    return "done";
  case PlanningJobState::kFailed:
    return "failed";
  case PlanningJobState::kCancelled:
    return "cancelled";
  }
  return "failed";
}

CanonicalValue PlanningJobValue(const PlanningJobSnapshot &job) {
  const bool done = job.state == PlanningJobState::kDone && job.outcome;
  const bool failed = job.state == PlanningJobState::kFailed && job.outcome;
  return CanonicalValue(CanonicalValue::Object{
      {"error", failed ? *job.outcome : CanonicalValue(nullptr)},
      {"job_id", CanonicalValue(job.job_id)},
      {"kind", CanonicalValue(job.pick ? "pick" : "place")},
      {"planning_result", done ? *job.outcome : CanonicalValue(nullptr)},
      {"schema_version", CanonicalValue("manyforge.planning_job.v1")},
      {"state", CanonicalValue(std::string(PlanningJobStateName(job.state)))},
  });
}

CanonicalValue PlanningJobsStreamValue(const std::vector<PlanningJobStreamRow> &rows) {
  CanonicalValue::Array jobs;
  jobs.reserve(rows.size());
  for (const auto &row : rows) {
    jobs.emplace_back(CanonicalValue::Object{
        {"job_id", CanonicalValue(row.job_id)},
        {"kind", CanonicalValue(row.pick ? "pick" : "place")},
        {"program_run_id",
         row.program_run_id
             ? CanonicalValue(static_cast<double>(row.program_run_id->value()))
             : CanonicalValue(nullptr)},
        {"state", CanonicalValue(std::string(PlanningJobStateName(row.state)))},
    });
  }
  return CanonicalValue(std::move(jobs));
}

std::vector<PlanningJobStreamRow> PlanningJobTable::StreamRows() const {
  std::lock_guard lock(mutex_);
  std::vector<PlanningJobStreamRow> rows;
  rows.reserve(jobs_.size());
  for (const auto &job : jobs_) {
    if (job.released) continue;
    rows.push_back({job.snapshot.job_id, job.snapshot.pick, job.snapshot.state,
                    job.program_run_id});
  }
  return rows;
}

PlanningJobTable::Job *PlanningJobTable::FindLocked(const std::string_view job_id) {
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job &job) {
    return !job.released && job.snapshot.job_id == job_id;
  });
  return found == jobs_.end() ? nullptr : &*found;
}

const PlanningJobTable::Job *
PlanningJobTable::FindLocked(const std::string_view job_id) const {
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job &job) {
    return !job.released && job.snapshot.job_id == job_id;
  });
  return found == jobs_.end() ? nullptr : &*found;
}

core::Result<PlanningJobTable::Accepted>
PlanningJobTable::Accept(std::string job_id, const bool pick,
                         std::optional<core::ProgramRunNumber> program_run_id) {
  const NotifyStreamAfterUnlock notify;
  std::lock_guard lock(mutex_);
  if (FindLocked(job_id) != nullptr) {
    return core::Result<Accepted>::Err(core::ErrorDTO{
        "planning_job_exists",
        "a planning job of this name is held until it is released",
        {{"job_id", job_id}}});
  }
  if (jobs_.size() >= kMaximumPlanningJobs) {
    return core::Result<Accepted>::Err(core::ErrorDTO{
        "execution_planning_jobs_busy",
        "the execution owner already holds its bounded number of planning jobs",
        {}});
  }
  Job job;
  job.ticket = next_ticket_++;
  job.program_run_id = program_run_id;
  job.snapshot.job_id = std::move(job_id);
  job.snapshot.pick = pick;
  job.snapshot.state = PlanningJobState::kAccepted;
  Accepted accepted{job.ticket, job.snapshot};
  jobs_.push_back(std::move(job));
  return core::Result<Accepted>::Ok(std::move(accepted));
}

void PlanningJobTable::Withdraw(const std::uint64_t ticket) noexcept {
  const NotifyStreamAfterUnlock notify;
  std::lock_guard lock(mutex_);
  std::erase_if(jobs_, [&](const Job &job) { return job.ticket == ticket; });
}

std::optional<std::stop_source>
PlanningJobTable::Begin(const std::uint64_t ticket) noexcept {
  const NotifyStreamAfterUnlock notify;
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(),
                                  [&](const Job &job) { return job.ticket == ticket; });
  if (found == jobs_.end()) return std::nullopt;
  if (found->released) {
    jobs_.erase(found);
    return std::nullopt;
  }
  if (found->snapshot.state != PlanningJobState::kAccepted) return std::nullopt;
  found->snapshot.state = PlanningJobState::kPlanning;
  return found->stop;
}

void PlanningJobTable::Finish(const std::uint64_t ticket, const bool succeeded,
                              CanonicalValue outcome) {
  auto shared = std::make_shared<const CanonicalValue>(std::move(outcome));
  const NotifyStreamAfterUnlock notify;
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(),
                                  [&](const Job &job) { return job.ticket == ticket; });
  if (found == jobs_.end()) return;
  if (found->released) {
    jobs_.erase(found);
    return;
  }
  auto &snapshot = found->snapshot;
  if (succeeded) {
    snapshot.state = PlanningJobState::kDone;
    snapshot.outcome = std::move(shared);
  } else if (found->cancel_requested) {
    snapshot.state = PlanningJobState::kCancelled;
    snapshot.outcome.reset();
  } else {
    snapshot.state = PlanningJobState::kFailed;
    snapshot.outcome = std::move(shared);
  }
}

core::Result<PlanningJobSnapshot>
PlanningJobTable::Read(const std::string_view job_id) const {
  std::lock_guard lock(mutex_);
  const auto *job = FindLocked(job_id);
  if (job == nullptr) return core::Result<PlanningJobSnapshot>::Err(UnknownJob(job_id));
  return core::Result<PlanningJobSnapshot>::Ok(job->snapshot);
}

core::Result<PlanningJobSnapshot>
PlanningJobTable::Cancel(const std::string_view job_id) {
  const NotifyStreamAfterUnlock notify;
  // request_stop runs the job's stop callbacks, so it is called after unlock.
  std::optional<std::stop_source> stop;
  PlanningJobSnapshot snapshot;
  {
    std::lock_guard lock(mutex_);
    auto *job = FindLocked(job_id);
    if (job == nullptr) return core::Result<PlanningJobSnapshot>::Err(UnknownJob(job_id));
    if (!Finished(job->snapshot.state)) {
      job->cancel_requested = true;
      stop = job->stop;
      // Still waiting: the lane skips it (Begin), so it is cancelled now.
      if (job->snapshot.state == PlanningJobState::kAccepted)
        job->snapshot.state = PlanningJobState::kCancelled;
    }
    snapshot = job->snapshot;
  }
  if (stop) stop->request_stop();
  return core::Result<PlanningJobSnapshot>::Ok(std::move(snapshot));
}

core::Result<void> PlanningJobTable::Release(const std::string_view job_id) {
  const NotifyStreamAfterUnlock notify;
  std::lock_guard lock(mutex_);
  auto *job = FindLocked(job_id);
  if (job == nullptr) return core::Result<void>::Err(UnknownJob(job_id));
  if (!Finished(job->snapshot.state)) {
    return core::Result<void>::Err(core::ErrorDTO{
        "planning_job_unfinished",
        "a planning job is released only once it is done, failed or cancelled",
        {{"job_id", std::string(job_id)},
         {"state", std::string(PlanningJobStateName(job->snapshot.state))}}});
  }
  // A job cancelled while it waited may still have its lane entry; Begin
  // finds no row for its ticket and the lane skips it.
  const auto ticket = job->ticket;
  std::erase_if(jobs_, [&](const Job &row) { return row.ticket == ticket; });
  return core::Result<void>::Ok();
}

void PlanningJobTable::Preempt(
    const std::optional<core::ProgramRunNumber> ended_run) noexcept {
  const NotifyStreamAfterUnlock notify;
  // request_stop runs the jobs' stop callbacks, so it is called after unlock.
  std::array<std::optional<std::stop_source>, kMaximumPlanningJobs> stops;
  std::size_t stop_count = 0U;
  {
    std::lock_guard lock(mutex_);
    for (auto &job : jobs_) {
      if (!Finished(job.snapshot.state)) stops[stop_count++] = job.stop;
      if (ended_run && job.program_run_id == ended_run) job.released = true;
    }
    // A finished job of the ended run goes now (a lane entry left by a job
    // cancelled while it waited finds no row and is skipped).
    std::erase_if(jobs_, [](const Job &job) {
      return job.released && Finished(job.snapshot.state);
    });
  }
  for (std::size_t index = 0U; index < stop_count; ++index) stops[index]->request_stop();
}

} // namespace manyforge::roboplan_backend::host_detail
