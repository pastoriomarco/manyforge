#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <stdexcept>
#include <utility>

namespace manyforge::core {

namespace {

class AtomicFlagGuard {
 public:
  explicit AtomicFlagGuard(std::atomic_flag& flag) : flag_(flag) {}
  ~AtomicFlagGuard() { flag_.clear(std::memory_order_release); }

 private:
  std::atomic_flag& flag_;
};

bool source_may_be_non_expiring(ObservedRobotStateAuthorityProfile profile) noexcept {
  return profile == ObservedRobotStateAuthorityProfile::kCommandedSimulation ||
         profile == ObservedRobotStateAuthorityProfile::kPredictedNative;
}

void atomic_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                                          std::memory_order_relaxed)) {
  }
}

}  // namespace

const char* observed_robot_state_source_kind_name(ObservedRobotStateSourceKind kind) noexcept {
  switch (kind) {
    case ObservedRobotStateSourceKind::kActionFeedback:
      return "action_feedback";
    case ObservedRobotStateSourceKind::kControllerState:
      return "controller_state";
    case ObservedRobotStateSourceKind::kJointState:
      return "joint_state";
    case ObservedRobotStateSourceKind::kCommandedState:
      return "commanded_state";
    case ObservedRobotStateSourceKind::kPredictedState:
      return "predicted_state";
  }
  return "unknown";
}

const char* observed_robot_state_authority_profile_name(
    ObservedRobotStateAuthorityProfile profile) noexcept {
  switch (profile) {
    case ObservedRobotStateAuthorityProfile::kMeasured:
      return "measured";
    case ObservedRobotStateAuthorityProfile::kCommandedSimulation:
      return "commanded_simulation";
    case ObservedRobotStateAuthorityProfile::kPredictedNative:
      return "predicted_native";
  }
  return "unknown";
}

ObservedRobotStatePublication::SourceLane::SourceLane(ObservedRobotStateSourceConfig source_config,
                                                      std::size_t joint_count)
    : source(std::move(source_config)) {
  if (source.authority_profile_id.empty()) {
    source.authority_profile_id =
        observed_robot_state_authority_profile_name(source.authority_profile);
  }
  for (auto& slot : slots) {
    slot.positions_rad.resize(joint_count, std::numeric_limits<double>::quiet_NaN());
    slot.velocities_rad_s.resize(joint_count, std::numeric_limits<double>::quiet_NaN());
  }
}

ObservedRobotStatePublication::ReadView::ReadView(const ObservedRobotStatePublication* owner,
                                                  const SourceLane* lane, const Slot* slot) noexcept
    : owner_(owner), lane_(lane), slot_(slot) {}

ObservedRobotStatePublication::ReadView::ReadView(ReadView&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      lane_(std::exchange(other.lane_, nullptr)),
      slot_(std::exchange(other.slot_, nullptr)) {}

ObservedRobotStatePublication::ReadView& ObservedRobotStatePublication::ReadView::operator=(
    ReadView&& other) noexcept {
  if (this != &other) {
    release();
    owner_ = std::exchange(other.owner_, nullptr);
    lane_ = std::exchange(other.lane_, nullptr);
    slot_ = std::exchange(other.slot_, nullptr);
  }
  return *this;
}

ObservedRobotStatePublication::ReadView::~ReadView() {
  release();
}

void ObservedRobotStatePublication::ReadView::release() noexcept {
  if (slot_ != nullptr) {
    slot_->users.fetch_sub(1, std::memory_order_release);
  }
  owner_ = nullptr;
  lane_ = nullptr;
  slot_ = nullptr;
}

std::span<const std::string> ObservedRobotStatePublication::ReadView::joint_names() const noexcept {
  return owner_ != nullptr ? owner_->canonical_joint_names() : std::span<const std::string>{};
}

std::span<const double> ObservedRobotStatePublication::ReadView::positions_rad() const noexcept {
  return slot_ != nullptr ? std::span<const double>(slot_->positions_rad)
                          : std::span<const double>{};
}

std::span<const double> ObservedRobotStatePublication::ReadView::velocities_rad_s() const noexcept {
  return slot_ != nullptr ? std::span<const double>(slot_->velocities_rad_s)
                          : std::span<const double>{};
}

std::uint64_t ObservedRobotStatePublication::ReadView::sequence() const noexcept {
  return slot_ != nullptr ? slot_->sequence : 0;
}

std::int64_t ObservedRobotStatePublication::ReadView::source_timestamp_ns() const noexcept {
  return slot_ != nullptr ? slot_->source_timestamp_ns : 0;
}

std::int64_t ObservedRobotStatePublication::ReadView::received_timestamp_ns() const noexcept {
  return slot_ != nullptr ? slot_->received_timestamp_ns : 0;
}

std::int64_t ObservedRobotStatePublication::ReadView::freshness_budget_ns() const noexcept {
  return lane_ != nullptr ? lane_->source.freshness_budget_ns : 0;
}

bool ObservedRobotStatePublication::ReadView::complete() const noexcept {
  return slot_ != nullptr && slot_->complete;
}

bool ObservedRobotStatePublication::ReadView::velocities_complete() const noexcept {
  return slot_ != nullptr && slot_->velocities_complete;
}

std::string_view ObservedRobotStatePublication::ReadView::joint_order_token() const noexcept {
  return owner_ != nullptr ? owner_->joint_order_token() : std::string_view{};
}

std::string_view ObservedRobotStatePublication::ReadView::source_id() const noexcept {
  return lane_ != nullptr ? std::string_view(lane_->source.source_id) : std::string_view{};
}

ObservedRobotStateSourceKind ObservedRobotStatePublication::ReadView::source_kind() const noexcept {
  return lane_ != nullptr ? lane_->source.kind : ObservedRobotStateSourceKind::kJointState;
}

ObservedRobotStateAuthorityProfile ObservedRobotStatePublication::ReadView::authority_profile()
    const noexcept {
  return lane_ != nullptr ? lane_->source.authority_profile
                          : ObservedRobotStateAuthorityProfile::kMeasured;
}

std::string_view ObservedRobotStatePublication::ReadView::authority_profile_id() const noexcept {
  return lane_ != nullptr ? std::string_view(lane_->source.authority_profile_id)
                          : std::string_view{};
}

ObservedRobotStatePublication::ObservedRobotStatePublication(
    std::vector<std::string> canonical_joint_names,
    std::vector<ObservedRobotStateSourceConfig> precedence, std::size_t max_source_joint_count)
    : canonical_joint_names_(std::move(canonical_joint_names)),
      joint_order_token_(make_joint_order_token(canonical_joint_names_)),
      max_source_joint_count_(max_source_joint_count) {
  if (canonical_joint_names_.empty()) {
    throw std::invalid_argument("observed robot state publication requires canonical joints");
  }
  for (std::size_t i = 0; i < canonical_joint_names_.size(); ++i) {
    if (canonical_joint_names_[i].empty() ||
        std::find(canonical_joint_names_.begin(),
                  canonical_joint_names_.begin() + static_cast<std::ptrdiff_t>(i),
                  canonical_joint_names_[i]) !=
            canonical_joint_names_.begin() + static_cast<std::ptrdiff_t>(i)) {
      throw std::invalid_argument(
          "observed robot state canonical joints must be non-empty and unique");
    }
  }
  if (precedence.empty()) {
    throw std::invalid_argument("observed robot state publication requires source precedence");
  }
  if (max_source_joint_count_ < canonical_joint_names_.size()) {
    throw std::invalid_argument(
        "observed robot state source-joint bound is smaller than canonical order");
  }
  lanes_.reserve(precedence.size());
  for (std::size_t i = 0; i < precedence.size(); ++i) {
    auto& source = precedence[i];
    if (source.source_id.empty()) {
      source.source_id = observed_robot_state_source_kind_name(source.kind);
    }
    if (source.freshness_budget_ns < 0 || (source.freshness_budget_ns == 0 &&
                                           !source_may_be_non_expiring(source.authority_profile))) {
      throw std::invalid_argument(
          "measured observed-state sources require a positive freshness budget");
    }
    const auto duplicate =
        std::find_if(precedence.begin(), precedence.begin() + static_cast<std::ptrdiff_t>(i),
                     [&source](const auto& prior) { return prior.kind == source.kind; });
    if (duplicate != precedence.begin() + static_cast<std::ptrdiff_t>(i)) {
      throw std::invalid_argument("observed robot state source precedence contains duplicates");
    }
    lanes_.push_back(
        std::make_unique<SourceLane>(std::move(source), canonical_joint_names_.size()));
  }
}

ObservedRobotStatePublishResult ObservedRobotStatePublication::publish(
    ObservedRobotStateSourceKind source, std::span<const std::string> source_joint_names,
    std::span<const double> positions_rad, std::span<const double> velocities_rad_s,
    std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns) noexcept {
  SourceLane* lane = nullptr;
  for (const auto& candidate : lanes_) {
    if (candidate->source.kind == source) {
      lane = candidate.get();
      break;
    }
  }
  if (lane == nullptr) {
    return {false, false, false, 0, "source_not_configured"};
  }
  if (received_timestamp_ns < 0) {
    rejected_total_.fetch_add(1, std::memory_order_relaxed);
    return {false, false, false, 0, "invalid_received_timestamp"};
  }
  if (source_joint_names.size() > max_source_joint_count_) {
    rejected_total_.fetch_add(1, std::memory_order_relaxed);
    return {false, false, false, 0, "source_joint_count_exceeds_bound"};
  }
  if (lane->writer.test_and_set(std::memory_order_acquire)) {
    writer_busy_total_.fetch_add(1, std::memory_order_relaxed);
    lane->writer_busy_total.fetch_add(1, std::memory_order_relaxed);
    return {false, false, false, 0, "source_writer_busy"};
  }
  AtomicFlagGuard writer_guard(lane->writer);

  const auto latest_received = lane->latest_received_timestamp_ns.load(std::memory_order_acquire);
  if (latest_received >= 0 && received_timestamp_ns < latest_received) {
    rejected_total_.fetch_add(1, std::memory_order_relaxed);
    return {false, false, false, 0, "received_timestamp_regressed"};
  }

  const int current = lane->published_slot.load(std::memory_order_acquire);
  Slot* target = nullptr;
  int target_index = -1;
  for (std::size_t i = 0; i < lane->slots.size(); ++i) {
    if (static_cast<int>(i) == current) {
      continue;
    }
    std::int32_t expected = 0;
    if (lane->slots[i].users.compare_exchange_strong(expected, -1, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
      target = &lane->slots[i];
      target_index = static_cast<int>(i);
      break;
    }
  }
  if (target == nullptr) {
    slot_exhausted_total_.fetch_add(1, std::memory_order_relaxed);
    return {false, false, false, 0, "publication_slots_exhausted"};
  }

  // One writer owns this lane. Publish the history gap before overwriting
  // its bytes; readers either pin the old slot or observe this fence.
  if (target->sequence > lane->retired_sequence.load(std::memory_order_relaxed)) {
    lane->retired_sequence.store(target->sequence, std::memory_order_release);
  }
  std::fill(target->positions_rad.begin(), target->positions_rad.end(),
            std::numeric_limits<double>::quiet_NaN());
  std::fill(target->velocities_rad_s.begin(), target->velocities_rad_s.end(),
            std::numeric_limits<double>::quiet_NaN());
  // The NaN-filled preallocated slots are also the bounded seen markers, so
  // projection performs no callback-time allocation.
  bool source_shape_valid = source_joint_names.size() == positions_rad.size();
  for (std::size_t source_index = 0; source_shape_valid && source_index < source_joint_names.size();
       ++source_index) {
    const auto canonical = std::find(canonical_joint_names_.begin(), canonical_joint_names_.end(),
                                     source_joint_names[source_index]);
    if (canonical == canonical_joint_names_.end()) {
      continue;
    }
    const auto index =
        static_cast<std::size_t>(std::distance(canonical_joint_names_.begin(), canonical));
    const double position = positions_rad[source_index];
    if (!std::isfinite(position) || std::isfinite(target->positions_rad[index])) {
      source_shape_valid = false;
      break;
    }
    target->positions_rad[index] = position;
    if (source_index < velocities_rad_s.size() && std::isfinite(velocities_rad_s[source_index])) {
      target->velocities_rad_s[index] = velocities_rad_s[source_index];
    }
  }
  const bool complete =
      source_shape_valid && std::all_of(target->positions_rad.begin(), target->positions_rad.end(),
                                        [](double value) { return std::isfinite(value); });
  const bool velocities_complete =
      complete && velocities_rad_s.size() == source_joint_names.size() &&
      std::all_of(target->velocities_rad_s.begin(), target->velocities_rad_s.end(),
                  [](double value) { return std::isfinite(value); });
  target->sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
  target->source_timestamp_ns = source_timestamp_ns;
  target->received_timestamp_ns = received_timestamp_ns;
  target->complete = complete;
  target->velocities_complete = velocities_complete;
  // Commit the index while the target remains writer-owned. A reader that
  // observes the new index cannot pin the slot until every payload write is
  // visible; a reader that still observes an older index cannot mistake this
  // recycled slot for that older publication.
  lane->published_slot.store(target_index, std::memory_order_release);
  target->users.store(0, std::memory_order_release);
  const auto previous_received =
      lane->latest_received_timestamp_ns.exchange(received_timestamp_ns, std::memory_order_relaxed);
  if (previous_received >= 0 && received_timestamp_ns >= previous_received) {
    atomic_max(lane->max_interarrival_ns,
               static_cast<std::uint64_t>(received_timestamp_ns - previous_received));
  }
  lane->published_total.fetch_add(1, std::memory_order_relaxed);
  if (!complete) {
    lane->incomplete_total.fetch_add(1, std::memory_order_relaxed);
  }
  published_total_.fetch_add(1, std::memory_order_relaxed);
  return {true, complete, velocities_complete, target->sequence,
          complete ? "" : "incomplete_or_invalid_sample"};
}

ObservedRobotStatePublication::ReadView ObservedRobotStatePublication::acquire_slot(
    const SourceLane& lane, std::size_t index) const noexcept {
  if (index >= lane.slots.size()) {
    return {};
  }
  const Slot& slot = lane.slots[index];
  std::int32_t users = slot.users.load(std::memory_order_acquire);
  while (users >= 0) {
    if (users == std::numeric_limits<std::int32_t>::max()) {
      return {};
    }
    if (slot.users.compare_exchange_weak(users, users + 1, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
      return ReadView(this, &lane, &slot);
    }
  }
  return {};
}

ObservedRobotStatePublication::ReadView ObservedRobotStatePublication::acquire_lane(
    const SourceLane& lane) const noexcept {
  for (int attempt = 0; attempt < 3; ++attempt) {
    const int index = lane.published_slot.load(std::memory_order_acquire);
    if (index < 0 || index >= static_cast<int>(lane.slots.size())) {
      return {};
    }
    auto view = acquire_slot(lane, static_cast<std::size_t>(index));
    if (view && lane.published_slot.load(std::memory_order_acquire) == index) {
      return view;
    }
    view = ReadView{};
  }
  reader_retry_exhausted_total_.fetch_add(1, std::memory_order_relaxed);
  return {};
}

ObservedRobotStatePublication::ReadView ObservedRobotStatePublication::acquire(
    std::int64_t now_ns) const noexcept {
  ReadView complete_fallback;
  ReadView any_fallback;
  for (const auto& lane : lanes_) {
    auto candidate = acquire_lane(*lane);
    if (!candidate) {
      continue;
    }
    const auto budget = candidate.freshness_budget_ns();
    const auto received = candidate.received_timestamp_ns();
    const bool fresh = budget == 0 || (now_ns >= received && now_ns - received <= budget);
    if (candidate.complete() && fresh) {
      return candidate;
    }
    if (candidate.complete() && !complete_fallback) {
      complete_fallback = std::move(candidate);
    } else if (!any_fallback) {
      any_fallback = std::move(candidate);
    }
  }
  return complete_fallback ? std::move(complete_fallback) : std::move(any_fallback);
}

ObservedRobotStatePublication::ReadView ObservedRobotStatePublication::acquire_as_of(
    std::int64_t now_ns) const noexcept {
  for (const auto& lane : lanes_) {
    auto latest = acquire_lane(*lane);
    if (!latest) continue;
    const auto eligible = [now_ns](const ReadView& view) {
      return view.sequence() > 0 && view.complete() && view.received_timestamp_ns() >= 0 &&
             view.received_timestamp_ns() <= now_ns &&
             (view.freshness_budget_ns() == 0 ||
              now_ns - view.received_timestamp_ns() <= view.freshness_budget_ns());
    };
    if (latest.received_timestamp_ns() <= now_ns) {
      // Normal reads need only the current slot. A known latest invalidation
      // must never resurrect an older complete sample in the same lane.
      if (eligible(latest)) return latest;
      continue;
    }
    // The writer crossed the caller's read boundary. Only this case needs
    // the small retained-slot scan. A failed pin makes selection unproven.
    ReadView selected;
    bool selection_proven = true;
    for (std::size_t index = 0; index < lane->slots.size(); ++index) {
      auto candidate = acquire_slot(*lane, index);
      if (!candidate) {
        selection_proven = false;
        reader_retry_exhausted_total_.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      if (candidate.sequence() == 0 || candidate.received_timestamp_ns() < 0 ||
          candidate.received_timestamp_ns() > now_ns) {
        continue;
      }
      if (!selected || candidate.received_timestamp_ns() > selected.received_timestamp_ns() ||
          (candidate.received_timestamp_ns() == selected.received_timestamp_ns() &&
           candidate.sequence() > selected.sequence())) {
        selected = std::move(candidate);
      }
    }
    // Validate the newest sample at the boundary, not an earlier complete
    // sample hidden behind a newer source invalidation.
    if (selection_proven && selected && eligible(selected) &&
        selected.sequence() > lane->retired_sequence.load(std::memory_order_acquire)) {
      return selected;
    }
  }
  return {};
}

ObservedRobotStatePublication::ReadView ObservedRobotStatePublication::acquire_latest()
    const noexcept {
  for (const auto& lane : lanes_) {
    auto candidate = acquire_lane(*lane);
    if (candidate) {
      return candidate;
    }
  }
  return {};
}

ObservedRobotStatePublicationMetrics ObservedRobotStatePublication::metrics() const noexcept {
  return {
      published_total_.load(std::memory_order_relaxed),
      rejected_total_.load(std::memory_order_relaxed),
      writer_busy_total_.load(std::memory_order_relaxed),
      slot_exhausted_total_.load(std::memory_order_relaxed),
      reader_retry_exhausted_total_.load(std::memory_order_relaxed),
  };
}

std::optional<ObservedRobotStateSnapshotDTO> ObservedRobotStatePublication::snapshot(
    std::int64_t now_ns) const {
  auto view = acquire(now_ns);
  if (!view) {
    return std::nullopt;
  }
  ObservedRobotStateSnapshotDTO dto;
  dto.sequence = view.sequence();
  dto.joint_names.assign(view.joint_names().begin(), view.joint_names().end());
  dto.positions_rad.assign(view.positions_rad().begin(), view.positions_rad().end());
  if (view.velocities_complete()) {
    dto.velocities_rad_s.emplace(view.velocities_rad_s().begin(), view.velocities_rad_s().end());
  }
  dto.source_id = std::string(view.source_id());
  dto.source_kind = view.source_kind();
  dto.source_timestamp_ns = view.source_timestamp_ns();
  dto.received_timestamp_ns = view.received_timestamp_ns();
  dto.authority_profile = std::string(view.authority_profile_id());
  dto.complete = view.complete();
  return dto;
}

std::optional<ObservedRobotStateSnapshotDTO> ObservedRobotStatePublication::snapshot_latest()
    const {
  auto view = acquire_latest();
  if (!view) {
    return std::nullopt;
  }
  ObservedRobotStateSnapshotDTO dto;
  dto.sequence = view.sequence();
  dto.joint_names.assign(view.joint_names().begin(), view.joint_names().end());
  dto.positions_rad.assign(view.positions_rad().begin(), view.positions_rad().end());
  if (view.velocities_complete()) {
    dto.velocities_rad_s.emplace(view.velocities_rad_s().begin(), view.velocities_rad_s().end());
  }
  dto.source_id = std::string(view.source_id());
  dto.source_kind = view.source_kind();
  dto.source_timestamp_ns = view.source_timestamp_ns();
  dto.received_timestamp_ns = view.received_timestamp_ns();
  dto.authority_profile = std::string(view.authority_profile_id());
  dto.complete = view.complete();
  return dto;
}

std::vector<ObservedRobotStateSourceMetrics> ObservedRobotStatePublication::source_metrics() const {
  std::vector<ObservedRobotStateSourceMetrics> result;
  result.reserve(lanes_.size());
  for (const auto& lane : lanes_) {
    result.push_back(ObservedRobotStateSourceMetrics{
        lane->source.kind,
        lane->source.source_id,
        lane->published_total.load(std::memory_order_relaxed),
        lane->incomplete_total.load(std::memory_order_relaxed),
        lane->writer_busy_total.load(std::memory_order_relaxed),
        lane->latest_received_timestamp_ns.load(std::memory_order_relaxed),
        lane->max_interarrival_ns.load(std::memory_order_relaxed),
    });
  }
  return result;
}

std::vector<ObservedRobotStateSourceConfig> ObservedRobotStatePublication::source_configs() const {
  std::vector<ObservedRobotStateSourceConfig> result;
  result.reserve(lanes_.size());
  for (const auto& lane : lanes_) {
    result.push_back(lane->source);
  }
  return result;
}

std::string ObservedRobotStatePublication::make_joint_order_token(
    std::span<const std::string> joint_names) {
  std::string token;
  for (const auto& joint : joint_names) {
    if (!token.empty()) {
      token.push_back('\x1f');
    }
    token.append(joint);
  }
  return token;
}

}  // namespace manyforge::core
