#include <manyforge_core/execution_progress_publication.hpp>
#include <stdexcept>
#include <utility>

namespace manyforge::core {
namespace {

bool profile_may_be_non_expiring(ExecutionProgressAuthorityProfile profile) noexcept {
  return profile == ExecutionProgressAuthorityProfile::kCommandedSimulation ||
         profile == ExecutionProgressAuthorityProfile::kPredictedNative;
}

}  // namespace

const char* execution_progress_source_kind_name(ExecutionProgressSourceKind kind) noexcept {
  switch (kind) {
    case ExecutionProgressSourceKind::kBackendCommanded:
      return "backend_commanded";
    case ExecutionProgressSourceKind::kMeasuredProjection:
      return "measured_projection";
  }
  return "unknown";
}

const char* execution_progress_authority_profile_name(
    ExecutionProgressAuthorityProfile profile) noexcept {
  switch (profile) {
    case ExecutionProgressAuthorityProfile::kCommandedSimulation:
      return "commanded_simulation";
    case ExecutionProgressAuthorityProfile::kPredictedNative:
      return "predicted_native";
    case ExecutionProgressAuthorityProfile::kMeasuredPhysical:
      return "measured_physical";
  }
  return "unknown";
}

ExecutionProgressPublication::ExecutionProgressPublication(
    ExecutionProgressSourceKind source_kind, ExecutionProgressAuthorityProfile profile,
    std::string authority_profile_id, std::int64_t freshness_budget_ns)
    : source_kind_(source_kind),
      authority_profile_(profile),
      authority_profile_id_(authority_profile_id.empty()
                                ? execution_progress_authority_profile_name(profile)
                                : std::move(authority_profile_id)),
      freshness_budget_ns_(freshness_budget_ns) {
  if (freshness_budget_ns_ < 0 ||
      (freshness_budget_ns_ == 0 && !profile_may_be_non_expiring(authority_profile_))) {
    throw std::invalid_argument("measured execution progress requires a positive freshness budget");
  }
  if (source_kind_ == ExecutionProgressSourceKind::kMeasuredProjection &&
      authority_profile_ != ExecutionProgressAuthorityProfile::kMeasuredPhysical) {
    throw std::invalid_argument("measured progress source requires measured-physical authority");
  }
  if (source_kind_ == ExecutionProgressSourceKind::kBackendCommanded &&
      authority_profile_ == ExecutionProgressAuthorityProfile::kMeasuredPhysical) {
    throw std::invalid_argument("backend-commanded progress cannot claim measured authority");
  }
}

void ExecutionProgressPublication::begin_write() noexcept {
  epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void ExecutionProgressPublication::end_write() noexcept {
  epoch_.fetch_add(1, std::memory_order_release);
}

ExecutionProgressPublishResult ExecutionProgressPublication::publish(
    std::uint64_t execution_generation, std::int64_t trajectory_time_ns,
    std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns) noexcept {
  return publish_impl(execution_generation, trajectory_time_ns,
                      source_timestamp_ns, received_timestamp_ns,
                      /*terminal=*/false);
}

ExecutionProgressPublishResult ExecutionProgressPublication::publish_terminal(
    std::uint64_t execution_generation, std::int64_t trajectory_time_ns,
    std::int64_t source_timestamp_ns,
    std::int64_t received_timestamp_ns) noexcept {
  return publish_impl(execution_generation, trajectory_time_ns,
                      source_timestamp_ns, received_timestamp_ns,
                      /*terminal=*/true);
}

ExecutionProgressPublishResult ExecutionProgressPublication::publish_impl(
    std::uint64_t execution_generation, std::int64_t trajectory_time_ns,
    std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns,
    bool terminal) noexcept {
  if (execution_generation == 0 || trajectory_time_ns < 0 || received_timestamp_ns < 0) {
    return {false, 0, "invalid_progress_sample"};
  }
  std::lock_guard<std::mutex> lock(writer_mutex_);
  const auto current_generation = execution_generation_.load(std::memory_order_relaxed);
  const auto current_time = trajectory_time_ns_.load(std::memory_order_relaxed);
  const bool current_valid = valid_.load(std::memory_order_relaxed);
  const bool current_terminal = terminal_.load(std::memory_order_relaxed);
  if (current_generation > execution_generation) {
    return {false, 0, "execution_generation_regressed"};
  }
  // Invalidation retires the current generation. A late callback from that
  // dispatch must not restore progress authority after stop/reset/terminal
  // retirement; only a strictly newer dispatch may publish again.
  if (!current_valid && current_generation != 0 &&
      current_generation == execution_generation) {
    return {false, 0, "execution_generation_retired"};
  }
  if (current_valid && current_generation == execution_generation &&
      current_terminal) {
    return terminal
               ? ExecutionProgressPublishResult{true,
                                                sequence_.load(
                                                    std::memory_order_relaxed),
                                                "already_terminal"}
               : ExecutionProgressPublishResult{
                     false, 0, "execution_generation_terminal"};
  }
  if (current_generation == execution_generation &&
      hazard_authority_claimed_) {
    return {false, 0, "execution_generation_hazard_claimed"};
  }
  if (current_valid && current_generation == execution_generation &&
      trajectory_time_ns < current_time) {
    return {false, 0, "trajectory_time_regressed"};
  }

  begin_write();
  const auto sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
  sequence_.store(sequence, std::memory_order_relaxed);
  execution_generation_.store(execution_generation, std::memory_order_relaxed);
  trajectory_time_ns_.store(trajectory_time_ns, std::memory_order_relaxed);
  source_timestamp_ns_.store(source_timestamp_ns, std::memory_order_relaxed);
  received_timestamp_ns_.store(received_timestamp_ns, std::memory_order_relaxed);
  valid_.store(true, std::memory_order_relaxed);
  terminal_.store(terminal, std::memory_order_relaxed);
  hazard_authority_claimed_ = false;
  end_write();
  return {true, sequence, ""};
}

ExecutionProgressHazardClaimResult
ExecutionProgressPublication::claim_hazard_authority(
    std::uint64_t execution_generation) noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (execution_generation == 0 ||
      execution_generation_.load(std::memory_order_relaxed) !=
          execution_generation) {
    return ExecutionProgressHazardClaimResult::kGenerationMismatch;
  }
  if (valid_.load(std::memory_order_relaxed) &&
      terminal_.load(std::memory_order_relaxed)) {
    return ExecutionProgressHazardClaimResult::kTerminal;
  }
  hazard_authority_claimed_ = true;
  return ExecutionProgressHazardClaimResult::kClaimed;
}

void ExecutionProgressPublication::invalidate() noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  begin_write();
  valid_.store(false, std::memory_order_relaxed);
  terminal_.store(false, std::memory_order_relaxed);
  hazard_authority_claimed_ = false;
  sequence_.store(next_sequence_.fetch_add(1, std::memory_order_relaxed),
                  std::memory_order_relaxed);
  end_write();
}

bool ExecutionProgressPublication::invalidate_generation(
    std::uint64_t expected_generation) noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (execution_generation_.load(std::memory_order_relaxed) != expected_generation) {
    return false;
  }
  begin_write();
  valid_.store(false, std::memory_order_relaxed);
  terminal_.store(false, std::memory_order_relaxed);
  hazard_authority_claimed_ = false;
  sequence_.store(next_sequence_.fetch_add(1, std::memory_order_relaxed),
                  std::memory_order_relaxed);
  end_write();
  return true;
}

ExecutionProgressCursorSnapshotDTO ExecutionProgressPublication::acquire() const noexcept {
  ExecutionProgressCursorSnapshotDTO snapshot;
  snapshot.source_kind = source_kind_;
  snapshot.authority_profile = authority_profile_id_;
  snapshot.freshness_budget_ns = freshness_budget_ns_;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto before = epoch_.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) {
      continue;
    }
    snapshot.sequence = sequence_.load(std::memory_order_relaxed);
    snapshot.execution_generation = execution_generation_.load(std::memory_order_relaxed);
    snapshot.trajectory_time_ns = trajectory_time_ns_.load(std::memory_order_relaxed);
    snapshot.source_timestamp_ns = source_timestamp_ns_.load(std::memory_order_relaxed);
    snapshot.received_timestamp_ns = received_timestamp_ns_.load(std::memory_order_relaxed);
    snapshot.valid = valid_.load(std::memory_order_relaxed);
    snapshot.terminal = terminal_.load(std::memory_order_relaxed);
    const auto after = epoch_.load(std::memory_order_acquire);
    if (before == after && (after & 1u) == 0u) {
      return snapshot;
    }
  }
  snapshot.valid = false;
  return snapshot;
}

}  // namespace manyforge::core
