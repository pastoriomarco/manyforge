#include <manyforge_core/execution_progress_publication.hpp>
#include <stdexcept>
#include <utility>

namespace manyforge::core {
namespace {

// The hazard word (OD-175 D2): generation << 2 | state.
constexpr std::uint64_t kHazardOpen = 0U;
constexpr std::uint64_t kHazardClaimed = 1U;
constexpr std::uint64_t kHazardClosed = 2U;
constexpr std::uint64_t kHazardStateMask = 3U;

constexpr std::uint64_t hazard_word(std::uint64_t generation, std::uint64_t state) noexcept {
  return (generation << 2U) | state;
}
constexpr std::uint64_t hazard_generation(std::uint64_t word) noexcept { return word >> 2U; }
constexpr std::uint64_t hazard_state(std::uint64_t word) noexcept {
  return word & kHazardStateMask;
}

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

void ExecutionProgressPublication::store_locked() noexcept {
  const unsigned index = latest_slot_.load(std::memory_order_relaxed) ^ 1u;
  Slot& slot = slots_[index];
  // Fence-free seqlock (Boehm, "Can Seqlocks Get Along with Programming
  // Language Memory Models?", MSPC 2012): field stores are release and the
  // reader's field loads acquire, so a reader that sees any new field value
  // also sees the odd epoch. ThreadSanitizer models this; it does not model
  // standalone fences (gcc 13 -Werror=tsan).
  slot.epoch.fetch_add(1, std::memory_order_relaxed);
  slot.sequence.store(current_.sequence, std::memory_order_release);
  slot.execution_generation.store(current_.execution_generation, std::memory_order_release);
  slot.trajectory_time_ns.store(current_.trajectory_time_ns, std::memory_order_release);
  slot.source_timestamp_ns.store(current_.source_timestamp_ns, std::memory_order_release);
  slot.received_timestamp_ns.store(current_.received_timestamp_ns, std::memory_order_release);
  slot.valid.store(current_.valid, std::memory_order_release);
  slot.terminal.store(current_.terminal, std::memory_order_release);
  slot.epoch.fetch_add(1, std::memory_order_release);
  latest_slot_.store(index, std::memory_order_release);
}

ExecutionProgressPublishResult ExecutionProgressPublication::publish(
    std::uint64_t execution_generation, std::int64_t trajectory_time_ns,
    std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns) noexcept {
  return publish_impl(execution_generation, trajectory_time_ns, source_timestamp_ns,
                      received_timestamp_ns,
                      /*terminal=*/false);
}

ExecutionProgressPublishResult ExecutionProgressPublication::publish_terminal(
    std::uint64_t execution_generation, std::int64_t trajectory_time_ns,
    std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns) noexcept {
  return publish_impl(execution_generation, trajectory_time_ns, source_timestamp_ns,
                      received_timestamp_ns,
                      /*terminal=*/true);
}

ExecutionProgressPublishResult ExecutionProgressPublication::publish_impl(
    std::uint64_t execution_generation, std::int64_t trajectory_time_ns,
    std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns, bool terminal) noexcept {
  if (execution_generation == 0 || trajectory_time_ns < 0 || received_timestamp_ns < 0) {
    return {false, 0, "invalid_progress_sample"};
  }
  std::lock_guard<std::mutex> lock(writer_mutex_);
  const auto current_generation = current_.execution_generation;
  const auto current_time = current_.trajectory_time_ns;
  const bool current_valid = current_.valid;
  const bool current_terminal = current_.terminal;
  if (current_generation > execution_generation) {
    return {false, 0, "execution_generation_regressed"};
  }
  // Invalidation retires the current generation. A late callback from that
  // dispatch must not restore progress authority after stop/reset/terminal
  // retirement; only a strictly newer dispatch may publish again.
  if (!current_valid && current_generation != 0 && current_generation == execution_generation) {
    return {false, 0, "execution_generation_retired"};
  }
  if (current_valid && current_generation == execution_generation && current_terminal) {
    return terminal
               ? ExecutionProgressPublishResult{true, current_.sequence, "already_terminal"}
               : ExecutionProgressPublishResult{false, 0, "execution_generation_terminal"};
  }
  auto word = hazard_word_.load(std::memory_order_acquire);
  if (current_generation == execution_generation && hazard_generation(word) == execution_generation &&
      hazard_state(word) == kHazardClaimed) {
    return {false, 0, "execution_generation_hazard_claimed"};
  }
  if (current_valid && current_generation == execution_generation &&
      trajectory_time_ns < current_time) {
    return {false, 0, "trajectory_time_regressed"};
  }

  current_ = Cursor{next_sequence_++, execution_generation, trajectory_time_ns,
                    source_timestamp_ns, received_timestamp_ns, true, terminal};
  // A newer generation opens its own word; it may replace the previous
  // generation's closed word. A same-generation publication leaves the word
  // alone, so "closed" survives it (OD-175 D2). Only the close races this
  // compare-and-swap, and it never changes a word of the new generation.
  if (hazard_generation(word) != execution_generation) {
    while (!hazard_word_.compare_exchange_weak(word, hazard_word(execution_generation, kHazardOpen),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
    }
  }
  store_locked();
  return {true, current_.sequence, ""};
}

std::optional<bool> ExecutionProgressPublication::hazard_authority_claimed(
    const std::uint64_t execution_generation) const noexcept {
  const auto word = hazard_word_.load(std::memory_order_acquire);
  if (execution_generation == 0 || hazard_generation(word) != execution_generation)
    return std::nullopt;
  return hazard_state(word) == kHazardClaimed;
}

ExecutionProgressHazardClaimResult ExecutionProgressPublication::claim_hazard_authority(
    std::uint64_t execution_generation) noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (execution_generation == 0 || current_.execution_generation != execution_generation) {
    return ExecutionProgressHazardClaimResult::kGenerationMismatch;
  }
  if (current_.valid && current_.terminal) {
    return ExecutionProgressHazardClaimResult::kTerminal;
  }
  // One compare-and-swap against the take-over's close (OD-175 D2).
  auto expected = hazard_word(execution_generation, kHazardOpen);
  if (hazard_word_.compare_exchange_strong(expected,
                                           hazard_word(execution_generation, kHazardClaimed),
                                           std::memory_order_acq_rel, std::memory_order_acquire))
    return ExecutionProgressHazardClaimResult::kClaimed;
  // Already claimed: idempotent, as before. Closed: the hand-over decided first.
  return expected == hazard_word(execution_generation, kHazardClaimed)
             ? ExecutionProgressHazardClaimResult::kClaimed
             : ExecutionProgressHazardClaimResult::kGenerationMismatch;
}

bool ExecutionProgressPublication::close_hazard_authority(
    const std::uint64_t execution_generation) noexcept {
  if (execution_generation == 0) return false;
  auto expected = hazard_word(execution_generation, kHazardOpen);
  return hazard_word_.compare_exchange_strong(expected,
                                              hazard_word(execution_generation, kHazardClosed),
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
}

void ExecutionProgressPublication::release_hazard_claim_locked() noexcept {
  // Claimed to open only; open and closed are kept (OD-175 D2). The close
  // never changes a claimed word, so this single compare-and-swap cannot lose.
  auto word = hazard_word_.load(std::memory_order_acquire);
  if (hazard_state(word) != kHazardClaimed) return;
  (void)hazard_word_.compare_exchange_strong(
      word, hazard_word(hazard_generation(word), kHazardOpen), std::memory_order_acq_rel,
      std::memory_order_acquire);
}

void ExecutionProgressPublication::invalidate() noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  current_.valid = false;
  current_.terminal = false;
  current_.sequence = next_sequence_++;
  release_hazard_claim_locked();
  store_locked();
}

bool ExecutionProgressPublication::invalidate_generation(
    std::uint64_t expected_generation) noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (current_.execution_generation != expected_generation) {
    return false;
  }
  current_.valid = false;
  current_.terminal = false;
  current_.sequence = next_sequence_++;
  release_hazard_claim_locked();
  store_locked();
  return true;
}

ExecutionProgressCursorSnapshotDTO ExecutionProgressPublication::acquire() const noexcept {
  ExecutionProgressCursorSnapshotDTO snapshot;
  snapshot.source_kind = source_kind_;
  snapshot.authority_profile = authority_profile_id_;
  snapshot.freshness_budget_ns = freshness_budget_ns_;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const Slot& slot = slots_[latest_slot_.load(std::memory_order_acquire)];
    const auto before = slot.epoch.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) {
      continue;
    }
    // Acquire field loads pair with the writer's release field stores (see
    // store_locked) and keep the epoch re-check below after every field load.
    snapshot.sequence = slot.sequence.load(std::memory_order_acquire);
    snapshot.execution_generation = slot.execution_generation.load(std::memory_order_acquire);
    snapshot.trajectory_time_ns = slot.trajectory_time_ns.load(std::memory_order_acquire);
    snapshot.source_timestamp_ns = slot.source_timestamp_ns.load(std::memory_order_acquire);
    snapshot.received_timestamp_ns = slot.received_timestamp_ns.load(std::memory_order_acquire);
    snapshot.valid = slot.valid.load(std::memory_order_acquire);
    snapshot.terminal = slot.terminal.load(std::memory_order_acquire);
    if (slot.epoch.load(std::memory_order_relaxed) == before) {
      return snapshot;
    }
  }
  snapshot.valid = false;
  return snapshot;
}

}  // namespace manyforge::core
