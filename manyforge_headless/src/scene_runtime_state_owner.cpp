#include "manyforge_headless/scene_runtime_state_owner.hpp"

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <utility>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumIdentifierBytes = 256U;
constexpr std::size_t kMaximumReasonBytes = 1024U;
constexpr std::size_t kMaximumErrorBytes = 32U * 1024U;
constexpr std::size_t kMaximumPosePayloadBytes = 64U * 1024U;

[[noreturn]] void Fail(const std::string &detail) {
  throw SceneRuntimeStateError("scene runtime state owner: " + detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

bool IsBoundedText(const std::string &value, const std::size_t maximum_bytes,
                   const bool allow_empty = false) noexcept {
  return (allow_empty || !value.empty()) && value.size() <= maximum_bytes;
}

void ValidateText(const std::string &value, const std::string_view label,
                  const std::size_t maximum_bytes,
                  const bool allow_empty = false) {
  Require(IsBoundedText(value, maximum_bytes, allow_empty),
          std::string(label) + " violates its byte bound");
  Require(content_identity::is_utf8_nfc(value),
          std::string(label) + " must be valid NFC UTF-8");
}

void ValidateSafeInteger(const std::uint64_t value,
                         const std::string_view label,
                         const bool nonzero = false) {
  Require(value <= kMaximumSafeInteger && (!nonzero || value != 0U),
          std::string(label) + " must be an I-JSON safe integer");
}

void ValidateSafeInteger(const std::int64_t value,
                         const std::string_view label) {
  Require(value >= 0,
          std::string(label) + " must be a nonnegative int64");
}

void ValidateIdentity(const SceneRuntimeIdentity &identity) {
  Require(identity.valid(), "runtime identity is invalid");
  ValidateText(identity.retained_bundle_id, "retained_bundle_id",
               kMaximumIdentifierBytes);
  ValidateText(identity.graph_revision, "graph_revision",
               kMaximumIdentifierBytes);
  ValidateSafeInteger(identity.deployment_generation, "deployment_generation");
  ValidateSafeInteger(identity.program_generation, "program_generation");
  ValidateSafeInteger(identity.runtime_generation, "runtime_generation", true);
}

void ValidateObservation(const PoseSourceObservation &observation) {
  Require(observation.valid(), "pose observation is invalid");
  ValidateText(observation.key.source_id, "pose source_id",
               kMaximumIdentifierBytes);
  ValidateText(observation.key.object_id, "pose object_id",
               kMaximumIdentifierBytes);
  ValidateText(observation.key.policy_id, "pose policy_id",
               kMaximumIdentifierBytes);
  ValidateText(observation.source_epoch, "pose source_epoch",
               kMaximumIdentifierBytes);
  ValidateSafeInteger(observation.source_epoch_order,
                      "pose source_epoch_order");
  ValidateSafeInteger(observation.source_sequence, "pose source_sequence");
  ValidateSafeInteger(observation.observed_at_ns, "pose observed_at_ns");
  ValidateSafeInteger(observation.expires_at_ns, "pose expires_at_ns");
  Require(observation.expires_at_ns >= observation.observed_at_ns,
          "pose observation expiry precedes observation time");
  Require(content_identity::canonical_json(observation.payload).size() <=
              kMaximumPosePayloadBytes,
          "pose observation payload exceeds its byte bound");
}

bool IsPending(const SceneInitializationState state) noexcept {
  return state == SceneInitializationState::kClosed ||
         state == SceneInitializationState::kResetting ||
         state == SceneInitializationState::kSeeding ||
         state == SceneInitializationState::kReplaying ||
         state == SceneInitializationState::kFailed;
}

bool IsImmediatePosePolicy(const std::string_view policy_id) noexcept {
  return policy_id == "continuous" || policy_id == "continuous_unattached";
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label,
                      const bool nonzero = false) {
  ValidateSafeInteger(value, label, nonzero);
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(const std::int64_t value, const std::string_view label) {
  ValidateSafeInteger(value, label);
  return content_identity::nonnegative_int64_value(value);
}

CanonicalValue OptionalString(const std::optional<std::string> &value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

CanonicalValue OptionalNumber(const std::optional<std::uint64_t> value,
                              const std::string_view label) {
  return value.has_value() ? Number(*value, label, true)
                           : CanonicalValue(nullptr);
}

CanonicalValue RuntimeIdentityValue(const SceneRuntimeIdentity &identity) {
  ValidateIdentity(identity);
  return CanonicalValue(CanonicalValue::Object{
      {"deployment_generation",
       Number(identity.deployment_generation, "deployment_generation")},
      {"graph_revision", CanonicalValue(identity.graph_revision)},
      {"program_generation",
       Number(identity.program_generation, "program_generation")},
      {"retained_bundle_id", CanonicalValue(identity.retained_bundle_id)},
      {"runtime_generation",
       Number(identity.runtime_generation, "runtime_generation", true)},
  });
}

CanonicalValue CountersValue(const SceneInitializationCounters &counters) {
  return CanonicalValue(CanonicalValue::Object{
      {"buffer_replaced",
       Number(counters.buffer_replaced, "counter.buffer_replaced")},
      {"buffered", Number(counters.buffered, "counter.buffered")},
      {"closed", Number(counters.closed, "counter.closed")},
      {"failed", Number(counters.failed, "counter.failed")},
      {"opened", Number(counters.opened, "counter.opened")},
      {"replayed", Number(counters.replayed, "counter.replayed")},
      {"retired", Number(counters.retired, "counter.retired")},
      {"stale_completion_rejected",
       Number(counters.stale_completion_rejected,
              "counter.stale_completion_rejected")},
  });
}

CanonicalValue PoseLifetimeValue(const PoseSourceLifetimeSnapshot &value) {
  ValidateObservation(value.observation);
  const auto &observation = value.observation;
  return CanonicalValue(CanonicalValue::Object{
      {"buffered", CanonicalValue(value.buffered)},
      {"eligible", CanonicalValue(observation.eligible)},
      {"expires_at_ns",
       Number(observation.expires_at_ns, "pose.expires_at_ns")},
      {"fresh", CanonicalValue(value.fresh)},
      {"object_id", CanonicalValue(observation.key.object_id)},
      {"observed_at_ns",
       Number(observation.observed_at_ns, "pose.observed_at_ns")},
      {"payload", observation.payload},
      {"policy_id", CanonicalValue(observation.key.policy_id)},
      {"quality", CanonicalValue(std::string(ToString(observation.quality)))},
      {"source_epoch", CanonicalValue(observation.source_epoch)},
      {"source_epoch_order",
       Number(observation.source_epoch_order, "pose.source_epoch_order")},
      {"source_id", CanonicalValue(observation.key.source_id)},
      {"source_sequence",
       Number(observation.source_sequence, "pose.source_sequence")},
  });
}

std::string ReplayBatchDigest(const SceneReplayBatch &batch) {
  CanonicalValue::Array observations;
  observations.reserve(batch.observations.size());
  for (const auto &observation : batch.observations) {
    ValidateObservation(observation);
    observations.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"eligible", CanonicalValue(observation.eligible)},
        {"expires_at_ns",
         Number(observation.expires_at_ns, "pose.expires_at_ns")},
        {"object_id", CanonicalValue(observation.key.object_id)},
        {"observed_at_ns",
         Number(observation.observed_at_ns, "pose.observed_at_ns")},
        {"payload", observation.payload},
        {"policy_id", CanonicalValue(observation.key.policy_id)},
        {"quality", CanonicalValue(std::string(ToString(observation.quality)))},
        {"source_epoch", CanonicalValue(observation.source_epoch)},
        {"source_epoch_order",
         Number(observation.source_epoch_order, "pose.source_epoch_order")},
        {"source_id", CanonicalValue(observation.key.source_id)},
        {"source_sequence",
         Number(observation.source_sequence, "pose.source_sequence")},
    }));
  }
  return content_identity::framed_sha256(
      "manyforge-scene-replay-batch", "v1",
      CanonicalValue(CanonicalValue::Object{
          {"initialization_generation",
           Number(batch.initialization_generation, "initialization_generation",
                  true)},
          {"initialization_token", CanonicalValue(batch.initialization_token)},
          {"observations", CanonicalValue(std::move(observations))},
          {"replay_id", Number(batch.replay_id, "replay_id", true)},
      }));
}

} // namespace

std::string_view ToString(const SceneInitializationState state) noexcept {
  switch (state) {
  case SceneInitializationState::kOpen:
    return "open";
  case SceneInitializationState::kClosed:
    return "closed";
  case SceneInitializationState::kResetting:
    return "resetting";
  case SceneInitializationState::kSeeding:
    return "seeding";
  case SceneInitializationState::kReplaying:
    return "replaying";
  case SceneInitializationState::kFailed:
    return "failed";
  case SceneInitializationState::kRetired:
    return "retired";
  }
  return {};
}

std::string_view ToString(const PoseSourceQuality quality) noexcept {
  switch (quality) {
  case PoseSourceQuality::kGood:
    return "good";
  case PoseSourceQuality::kUnknown:
    return "unknown";
  }
  return {};
}

std::string_view
ToString(const PoseObservationDisposition disposition) noexcept {
  switch (disposition) {
  case PoseObservationDisposition::kApplyNow:
    return "apply_now";
  case PoseObservationDisposition::kBuffered:
    return "buffered";
  case PoseObservationDisposition::kReplacedBuffered:
    return "replaced_buffered";
  case PoseObservationDisposition::kObservedOnly:
    return "observed_only";
  case PoseObservationDisposition::kDuplicate:
    return "duplicate";
  }
  return {};
}

bool SceneRuntimeIdentity::valid() const noexcept {
  return IsBoundedText(retained_bundle_id, kMaximumIdentifierBytes) &&
         IsBoundedText(graph_revision, kMaximumIdentifierBytes) &&
         runtime_generation != 0U &&
         deployment_generation <= kMaximumSafeInteger &&
         program_generation <= kMaximumSafeInteger &&
         runtime_generation <= kMaximumSafeInteger;
}

bool PoseObservationKey::valid() const noexcept {
  return IsBoundedText(source_id, kMaximumIdentifierBytes) &&
         IsBoundedText(object_id, kMaximumIdentifierBytes) &&
         IsBoundedText(policy_id, kMaximumIdentifierBytes);
}

bool operator<(const PoseObservationKey &left,
               const PoseObservationKey &right) noexcept {
  return std::tie(left.source_id, left.object_id, left.policy_id) <
         std::tie(right.source_id, right.object_id, right.policy_id);
}

bool PoseSourceObservation::valid() const noexcept {
  return key.valid() && IsBoundedText(source_epoch, kMaximumIdentifierBytes) &&
         source_epoch_order <= kMaximumSafeInteger &&
         source_sequence <= kMaximumSafeInteger && observed_at_ns >= 0 &&
         expires_at_ns >= observed_at_ns;
}

SceneRuntimeStateOwner::SceneRuntimeStateOwner(
    Clock clock, TokenFactory token_factory,
    const std::size_t maximum_pose_observation_keys)
    : clock_(std::move(clock)), token_factory_(std::move(token_factory)),
      maximum_pose_observation_keys_(maximum_pose_observation_keys) {
  Require(static_cast<bool>(clock_), "a clock is required");
  Require(static_cast<bool>(token_factory_), "a token factory is required");
  Require(maximum_pose_observation_keys_ != 0U,
          "pose observation capacity must be positive");
  started_at_ns_ = Now();
  updated_at_ns_ = started_at_ns_;
}

std::int64_t SceneRuntimeStateOwner::Now() const {
  const auto value = clock_();
  ValidateSafeInteger(value, "clock");
  return value;
}

void SceneRuntimeStateOwner::EstablishOpenRuntime(
    SceneRuntimeIdentity identity,
    const std::uint64_t initialization_generation, std::string reason) {
  ValidateIdentity(identity);
  ValidateSafeInteger(initialization_generation, "initialization_generation",
                      true);
  ValidateText(reason, "reason", kMaximumReasonBytes);
  const auto now = Now();
  std::lock_guard lock(mutex_);
  if (initialization_generation_ != 0U || runtime_identity_.has_value() ||
      initialization_token_.has_value()) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: open runtime is already established");
  }
  initialization_generation_ = initialization_generation;
  state_ = SceneInitializationState::kOpen;
  deferred_ = false;
  runtime_identity_ = std::move(identity);
  reason_ = std::move(reason);
  error_.reset();
  started_at_ns_ = now;
  updated_at_ns_ = now;
}

bool SceneRuntimeStateOwner::AdoptFinalizedOpenRuntime(
    SceneRuntimeIdentity identity,
    const std::uint64_t initialization_generation, std::string reason) {
  ValidateIdentity(identity);
  ValidateSafeInteger(initialization_generation, "initialization_generation",
                      true);
  ValidateText(reason, "reason", kMaximumReasonBytes);
  const auto now = Now();
  std::lock_guard lock(mutex_);
  if (state_ != SceneInitializationState::kOpen ||
      initialization_token_.has_value() || !runtime_identity_.has_value()) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: finalized runtime adoption requires one "
        "already-open runtime");
  }
  if (initialization_generation == initialization_generation_) {
    if (identity != *runtime_identity_) {
      throw SceneRuntimeStateConflictError(
          "scene runtime state owner: finalized runtime generation "
          "conflicts");
    }
    return false;
  }
  if (initialization_generation < initialization_generation_) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: finalized runtime generation is stale");
  }
  initialization_generation_ = initialization_generation;
  runtime_identity_ = std::move(identity);
  // Observations are scoped to the exact retained runtime identity.  An
  // externally finalized Apply may advance straight from one open runtime to
  // another without entering the reset/replay states, so retaining the old
  // rows here would make a still-unexpired pose visible under the successor
  // bundle.  Producers must reopen against the new identity and republish.
  pose_records_.clear();
  reason_ = std::move(reason);
  error_.reset();
  started_at_ns_ = now;
  updated_at_ns_ = now;
  return true;
}

std::string SceneRuntimeStateOwner::BeginInitialization(
    SceneRuntimeIdentity candidate, const bool deferred, std::string reason) {
  ValidateIdentity(candidate);
  ValidateText(reason, "reason", kMaximumReasonBytes);
  auto token = token_factory_();
  ValidateText(token, "initialization token", kMaximumIdentifierBytes);
  const auto now = Now();
  std::lock_guard lock(mutex_);
  if (initialization_token_.has_value() && token == *initialization_token_) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: token factory reused an initialization "
        "token");
  }
  if (initialization_generation_ >= kMaximumSafeInteger) {
    Fail("initialization generation is exhausted");
  }
  ++initialization_generation_;
  initialization_token_ = token;
  state_ = SceneInitializationState::kClosed;
  deferred_ = deferred;
  runtime_identity_ = std::move(candidate);
  reason_ = std::move(reason);
  error_.reset();
  started_at_ns_ = now;
  updated_at_ns_ = now;
  active_replay_id_.reset();
  active_replay_digest_.reset();
  for (auto &[key, record] : pose_records_) {
    static_cast<void>(key);
    record.buffered = false;
  }
  ++counters_.closed;
  return token;
}

void SceneRuntimeStateOwner::RequireCurrentTokenLocked(
    const std::string_view token) {
  if (token.empty() || !initialization_token_.has_value() ||
      token != *initialization_token_) {
    ++counters_.stale_completion_rejected;
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: stale initialization token");
  }
}

void SceneRuntimeStateOwner::PrepareReset(const std::string_view token,
                                          std::string reason) {
  ValidateText(reason, "reason", kMaximumReasonBytes);
  const auto now = Now();
  std::lock_guard lock(mutex_);
  RequireCurrentTokenLocked(token);
  if (state_ != SceneInitializationState::kClosed &&
      state_ != SceneInitializationState::kFailed) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: initialization is not resettable");
  }
  state_ = SceneInitializationState::kResetting;
  reason_ = std::move(reason);
  error_.reset();
  updated_at_ns_ = now;
  active_replay_id_.reset();
  active_replay_digest_.reset();
}

void SceneRuntimeStateOwner::MarkSeeded(const std::string_view token) {
  const auto now = Now();
  std::lock_guard lock(mutex_);
  RequireCurrentTokenLocked(token);
  if (state_ != SceneInitializationState::kResetting &&
      state_ != SceneInitializationState::kSeeding) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: reset has not reached scene seeding");
  }
  state_ = SceneInitializationState::kSeeding;
  updated_at_ns_ = now;
}

SceneReplayBatch
SceneRuntimeStateOwner::BeginReplay(const std::string_view token) {
  const auto now = Now();
  std::lock_guard lock(mutex_);
  RequireCurrentTokenLocked(token);
  if ((state_ != SceneInitializationState::kSeeding &&
       state_ != SceneInitializationState::kReplaying) ||
      active_replay_id_.has_value()) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: replay is not ready");
  }
  if (next_replay_id_ >= kMaximumSafeInteger) {
    Fail("replay identity is exhausted");
  }
  ++next_replay_id_;
  active_replay_id_ = next_replay_id_;
  state_ = SceneInitializationState::kReplaying;
  updated_at_ns_ = now;

  std::vector<PoseSourceObservation> observations;
  for (auto &[key, record] : pose_records_) {
    static_cast<void>(key);
    if (!record.buffered) {
      continue;
    }
    if (IsImmediatePosePolicy(record.observation.key.policy_id) &&
        record.observation.quality == PoseSourceQuality::kGood &&
        record.observation.eligible &&
        record.observation.expires_at_ns >= now) {
      observations.push_back(record.observation);
    }
    record.buffered = false;
  }
  SceneReplayBatch batch{
      initialization_generation_,
      *initialization_token_,
      *active_replay_id_,
      std::move(observations),
  };
  active_replay_digest_ = ReplayBatchDigest(batch);
  return batch;
}

void SceneRuntimeStateOwner::CompleteReplay(const SceneReplayBatch &batch,
                                            const std::size_t applied_count) {
  const auto now = Now();
  std::lock_guard lock(mutex_);
  RequireCurrentTokenLocked(batch.initialization_token);
  if (batch.initialization_generation != initialization_generation_ ||
      !active_replay_id_.has_value() || batch.replay_id != *active_replay_id_ ||
      state_ != SceneInitializationState::kReplaying ||
      !active_replay_digest_.has_value() ||
      ReplayBatchDigest(batch) != *active_replay_digest_) {
    ++counters_.stale_completion_rejected;
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: stale replay completion");
  }
  if (applied_count != batch.observations.size()) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: partial replay cannot report success");
  }
  counters_.replayed += static_cast<std::uint64_t>(applied_count);
  active_replay_id_.reset();
  active_replay_digest_.reset();
  const bool pending =
      std::any_of(pose_records_.begin(), pose_records_.end(),
                  [](const auto &entry) { return entry.second.buffered; });
  if (pending) {
    state_ = SceneInitializationState::kReplaying;
  } else {
    state_ = SceneInitializationState::kOpen;
    deferred_ = false;
    reason_ = "initialization_complete";
    error_.reset();
    ++counters_.opened;
  }
  updated_at_ns_ = now;
}

void SceneRuntimeStateOwner::RestoreReplayLocked(
    const SceneReplayBatch &batch) {
  for (const auto &observation : batch.observations) {
    const auto found = pose_records_.find(observation.key);
    if (found == pose_records_.end()) {
      pose_records_.emplace(observation.key, PoseRecord{observation, true});
      continue;
    }
    const auto &current = found->second.observation;
    if (current.source_epoch_order < observation.source_epoch_order ||
        (current.source_epoch_order == observation.source_epoch_order &&
         current.source_epoch == observation.source_epoch &&
         current.source_sequence <= observation.source_sequence)) {
      found->second = PoseRecord{observation, true};
    }
  }
}

void SceneRuntimeStateOwner::FailReplay(const SceneReplayBatch &batch,
                                        std::string error) {
  ValidateText(error, "error", kMaximumErrorBytes);
  const auto now = Now();
  std::lock_guard lock(mutex_);
  RequireCurrentTokenLocked(batch.initialization_token);
  if (batch.initialization_generation != initialization_generation_ ||
      !active_replay_id_.has_value() || batch.replay_id != *active_replay_id_ ||
      state_ != SceneInitializationState::kReplaying ||
      !active_replay_digest_.has_value() ||
      ReplayBatchDigest(batch) != *active_replay_digest_) {
    ++counters_.stale_completion_rejected;
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: stale replay failure");
  }
  RestoreReplayLocked(batch);
  active_replay_id_.reset();
  active_replay_digest_.reset();
  state_ = SceneInitializationState::kFailed;
  error_ = std::move(error);
  updated_at_ns_ = now;
  ++counters_.failed;
}

void SceneRuntimeStateOwner::FailInitialization(const std::string_view token,
                                                std::string error) {
  ValidateText(error, "error", kMaximumErrorBytes);
  const auto now = Now();
  std::lock_guard lock(mutex_);
  RequireCurrentTokenLocked(token);
  if (state_ == SceneInitializationState::kOpen ||
      state_ == SceneInitializationState::kRetired ||
      state_ == SceneInitializationState::kReplaying) {
    throw SceneRuntimeStateConflictError(
        "scene runtime state owner: initialization cannot fail in this state");
  }
  state_ = SceneInitializationState::kFailed;
  error_ = std::move(error);
  active_replay_id_.reset();
  active_replay_digest_.reset();
  updated_at_ns_ = now;
  ++counters_.failed;
}

void SceneRuntimeStateOwner::Retire(const std::string_view token,
                                    std::string reason) {
  ValidateText(reason, "reason", kMaximumReasonBytes);
  const auto now = Now();
  std::lock_guard lock(mutex_);
  RequireCurrentTokenLocked(token);
  state_ = SceneInitializationState::kRetired;
  reason_ = std::move(reason);
  active_replay_id_.reset();
  active_replay_digest_.reset();
  for (auto &[key, record] : pose_records_) {
    static_cast<void>(key);
    record.buffered = false;
  }
  updated_at_ns_ = now;
  ++counters_.retired;
}

PoseObservationAcceptance
SceneRuntimeStateOwner::ObservePose(PoseSourceObservation observation) {
  ValidateObservation(observation);
  const auto observation_key = observation.key;
  const auto now = Now();
  std::lock_guard lock(mutex_);
  auto found = pose_records_.find(observation_key);
  if (found != pose_records_.end()) {
    const auto &current = found->second.observation;
    if (observation.source_epoch_order < current.source_epoch_order) {
      throw SceneRuntimeStateConflictError(
          "scene runtime state owner: stale pose-source epoch");
    }
    if (observation.source_epoch_order == current.source_epoch_order) {
      if (observation.source_epoch != current.source_epoch) {
        throw SceneRuntimeStateConflictError(
            "scene runtime state owner: pose-source epoch order conflict");
      }
      if (observation.source_sequence < current.source_sequence) {
        throw SceneRuntimeStateConflictError(
            "scene runtime state owner: stale pose-source sequence");
      }
      if (observation.source_sequence == current.source_sequence) {
        if (observation == current) {
          return {PoseObservationDisposition::kDuplicate,
                  initialization_generation_,
                  observation.quality == PoseSourceQuality::kGood &&
                      observation.expires_at_ns >= now};
        }
        throw SceneRuntimeStateConflictError(
            "scene runtime state owner: pose-source sequence conflict");
      }
    }
  } else if (pose_records_.size() >= maximum_pose_observation_keys_) {
    throw SceneRuntimeStateCapacityError(
        "scene runtime state owner: pose observation capacity is exhausted");
  }

  const bool fresh = observation.quality == PoseSourceQuality::kGood &&
                     observation.expires_at_ns >= now;
  const bool can_apply = fresh && observation.eligible &&
                         IsImmediatePosePolicy(observation.key.policy_id);
  const bool was_buffered =
      found != pose_records_.end() && found->second.buffered;
  PoseObservationDisposition disposition =
      PoseObservationDisposition::kObservedOnly;
  bool buffered = false;
  if (state_ == SceneInitializationState::kOpen && can_apply) {
    disposition = PoseObservationDisposition::kApplyNow;
  } else if (IsPending(state_) && can_apply) {
    buffered = true;
    disposition = was_buffered ? PoseObservationDisposition::kReplacedBuffered
                               : PoseObservationDisposition::kBuffered;
    ++counters_.buffered;
    if (was_buffered) {
      ++counters_.buffer_replaced;
    }
  }
  pose_records_.insert_or_assign(observation_key,
                                 PoseRecord{std::move(observation), buffered});
  updated_at_ns_ = now;
  return {disposition, initialization_generation_, fresh};
}

SceneRuntimeStateSnapshot SceneRuntimeStateOwner::Snapshot() const {
  const auto now = Now();
  std::lock_guard lock(mutex_);
  std::vector<PoseSourceLifetimeSnapshot> pose_sources;
  pose_sources.reserve(pose_records_.size());
  for (const auto &[key, record] : pose_records_) {
    static_cast<void>(key);
    pose_sources.push_back(PoseSourceLifetimeSnapshot{
        record.observation,
        record.observation.quality == PoseSourceQuality::kGood &&
            record.observation.expires_at_ns >= now,
        record.buffered,
    });
  }
  return {
      initialization_generation_,
      initialization_token_,
      state_,
      deferred_,
      runtime_identity_,
      reason_,
      error_,
      started_at_ns_,
      updated_at_ns_,
      active_replay_id_,
      counters_,
      std::move(pose_sources),
  };
}

CanonicalValue
SceneRuntimeStateValue(const SceneRuntimeStateSnapshot &snapshot) {
  ValidateSafeInteger(snapshot.initialization_generation,
                      "initialization_generation");
  ValidateText(snapshot.reason, "reason", kMaximumReasonBytes, true);
  if (snapshot.initialization_token.has_value()) {
    ValidateText(*snapshot.initialization_token, "initialization_token",
                 kMaximumIdentifierBytes);
  }
  if (snapshot.error.has_value()) {
    ValidateText(*snapshot.error, "error", kMaximumErrorBytes);
  }
  CanonicalValue::Array pose_sources;
  pose_sources.reserve(snapshot.pose_sources.size());
  for (const auto &source : snapshot.pose_sources) {
    pose_sources.emplace_back(PoseLifetimeValue(source));
  }
  const auto identity = snapshot.runtime_identity.has_value()
                            ? RuntimeIdentityValue(*snapshot.runtime_identity)
                            : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"active_replay_id",
       OptionalNumber(snapshot.active_replay_id, "active_replay_id")},
      {"api_version",
       CanonicalValue(std::string(kSceneRuntimeStateSnapshotApiVersion))},
      {"counters", CountersValue(snapshot.counters)},
      {"deferred", CanonicalValue(snapshot.deferred)},
      {"error", OptionalString(snapshot.error)},
      {"initialization_generation",
       Number(snapshot.initialization_generation, "initialization_generation")},
      {"initialization_token", OptionalString(snapshot.initialization_token)},
      {"pose_sources", CanonicalValue(std::move(pose_sources))},
      {"reason", CanonicalValue(snapshot.reason)},
      {"runtime_identity", identity},
      {"started_at_ns", Number(snapshot.started_at_ns, "started_at_ns")},
      {"state", CanonicalValue(std::string(ToString(snapshot.state)))},
      {"updated_at_ns", Number(snapshot.updated_at_ns, "updated_at_ns")},
  });
}

} // namespace manyforge::headless
