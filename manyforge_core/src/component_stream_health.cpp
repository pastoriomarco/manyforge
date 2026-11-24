#include <manyforge_core/component_stream_health.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>

namespace manyforge::core {

namespace {

[[nodiscard]] bool Lower(const char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

}  // namespace

bool ValidStreamId(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= kMaxStreamIdBytes &&
         std::all_of(value.begin(), value.end(), [](const char c) {
           return Lower(c) || c == '_' || c == '.' || c == '/' || c == '-';
         });
}

namespace {

constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] std::string Truncated(const std::string_view text, std::size_t maximum) {
  if (text.size() <= maximum) return std::string(text);
  // Never split a UTF-8 sequence.
  while (maximum > 0U && (static_cast<unsigned char>(text[maximum]) & 0xC0U) == 0x80U) --maximum;
  return std::string(text.substr(0U, maximum));
}

using SlotKey = std::pair<ControllableComponentId, std::string>;

}  // namespace

struct StreamRecorder::Slot final {
  explicit Slot(ControllableComponentId component_in) : component(std::move(component_in)) {}

  ControllableComponentId component;
  StreamDeclarationDTO defaults;
  std::uint64_t serial{};
  std::shared_ptr<IClock> clock;
  std::weak_ptr<ComponentStreamHealth::Shared> owner;

  // Hot path (adapter receive threads).
  std::atomic<std::int64_t> last_stamp{kNever};
  std::atomic<std::int64_t> last_advance_ns{kNever};
  /// True while the registry classifies the stream live: samples then need
  /// no notification.
  std::atomic<bool> live{false};
  std::atomic<bool> notify_pending{false};

  // Registry-owned, under Shared::mutex.
  StreamDeclarationDTO effective;
  StreamLivenessStateDTO state{StreamLivenessStateDTO::kAbsent};
  std::int64_t declared_at_ns{};
  /// Absent past its start-up grace.
  bool absent_classified{};
};

struct ComponentStreamHealth::Shared final {
  Shared(std::shared_ptr<IClock> clock_in, IScheduler& scheduler_in) : clock(std::move(clock_in)), scheduler(scheduler_in) {}

  struct Deadline final {
    std::int64_t at_ns{};
    std::uint64_t serial{};
    std::weak_ptr<StreamRecorder::Slot> slot;
    bool operator>(const Deadline& other) const noexcept { return at_ns > other.at_ns; }
  };

  std::shared_ptr<IClock> clock;
  IScheduler& scheduler;

  mutable std::mutex mutex;
  std::map<SlotKey, std::shared_ptr<StreamRecorder::Slot>> slots;
  std::map<SlotKey, StreamParameterOverrideDTO> overrides;
  std::priority_queue<Deadline, std::vector<Deadline>, std::greater<>> deadlines;
  std::vector<std::weak_ptr<StreamRecorder::Slot>> fresh;
  std::set<ControllableComponentId> dirty;
  std::map<ControllableComponentId, ComponentStreamVerdictDTO> published;
  std::map<ControllableComponentId, std::shared_ptr<ComponentStreamStaleFlag>> watches;
  std::optional<ScheduleHandle> timer;
  std::int64_t timer_at_ns{};
  bool evaluation_scheduled{};
  std::atomic<bool> detached{false};
  std::mutex detach_mutex;
  std::uint64_t next_serial{1U};
  std::function<void(const std::string&)> log;

  /// Serializes log calls with SetLog.
  std::mutex log_mutex;

  void ScheduleEvaluationLocked(const std::shared_ptr<Shared>& self) {
    if (detached.load()) return;
    if (evaluation_scheduled) return;
    evaluation_scheduled = true;
    std::weak_ptr<Shared> weak = self;
    scheduler.schedule(
        clock->now(),
        [weak] {
          if (const auto shared = weak.lock()) Evaluate(shared);
        },
        SchedulerPriority::Monitoring);
  }

  void ApplyEffectiveLocked(StreamRecorder::Slot& slot) const {
    slot.effective = slot.defaults;
    const auto found = overrides.find({slot.component, slot.defaults.stream_id});
    if (found == overrides.end()) return;
    if (found->second.budget_ns) slot.effective.budget_ns = *found->second.budget_ns;
  }

  [[nodiscard]] static ErrorDTO Condition(const StreamRecorder::Slot& slot, const std::string_view code) {
    const auto& id = slot.defaults.stream_id;
    const std::string what = code == kComponentStreamStaleCode ? "is stale" : "has no sample";
    return {std::string(code),
            "Component " + slot.component.value() + " stream " + id + " " + what + " (budget " +
                std::to_string(slot.effective.budget_ns / 1'000'000) + " ms).",
            {{"stream_id", id}, {"budget_ns", std::to_string(slot.effective.budget_ns)}}};
  }

  /// The active condition code of one slot, if any.
  [[nodiscard]] static std::optional<std::string_view> ConditionCode(const StreamRecorder::Slot& slot) {
    if (slot.state == StreamLivenessStateDTO::kStale) return kComponentStreamStaleCode;
    if (slot.state == StreamLivenessStateDTO::kAbsent && slot.absent_classified)
      return kComponentStreamAbsentCode;
    return std::nullopt;
  }

  [[nodiscard]] ComponentStreamVerdictDTO VerdictLocked(const ControllableComponentId& component) const {
    ComponentStreamVerdictDTO verdict;
    for (auto it = slots.lower_bound({component, std::string()});
         it != slots.end() && it->first.first == component; ++it) {
      const auto& slot = *it->second;
      const auto code = ConditionCode(slot);
      if (!code || slot.effective.severity == StreamSeverityDTO::kWarning) continue;
      if (slot.effective.severity == StreamSeverityDTO::kFault && !verdict.fault_loss)
        verdict.fault_loss = Condition(slot, *code);
    }
    return verdict;
  }

  /// A `fault` stream of the component is stale.
  [[nodiscard]] bool StaleFaultLocked(const ControllableComponentId& component) const {
    for (auto it = slots.lower_bound({component, std::string()});
         it != slots.end() && it->first.first == component; ++it) {
      if (it->second->state == StreamLivenessStateDTO::kStale &&
          it->second->effective.severity == StreamSeverityDTO::kFault)
        return true;
    }
    return false;
  }

  void MarkStaleLocked(StreamRecorder::Slot& slot) {
    slot.state = StreamLivenessStateDTO::kStale;
    slot.absent_classified = false;
    dirty.insert(slot.component);
  }

  void PushDeadlineLocked(const std::shared_ptr<StreamRecorder::Slot>& slot, const std::int64_t at_ns) {
    deadlines.push({at_ns, slot->serial, slot});
  }

  [[nodiscard]] bool CurrentLocked(const std::shared_ptr<StreamRecorder::Slot>& slot) const {
    const auto found = slots.find({slot->component, slot->defaults.stream_id});
    return found != slots.end() && found->second == slot;
  }

  static void Evaluate(const std::shared_ptr<Shared>& self) {
    std::vector<std::pair<ControllableComponentId, ComponentStreamVerdictDTO>> changes;
    {
      std::lock_guard lock(self->mutex);
      self->evaluation_scheduled = false;
      if (self->detached.load()) return;
      const std::int64_t now = self->clock->now();
      // Samples that arrived while a stream was not live.
      for (const auto& weak : self->fresh) {
        const auto slot = weak.lock();
        if (!slot) continue;
        slot->notify_pending.store(false);
        if (!self->CurrentLocked(slot)) continue;
        const auto advanced = slot->last_advance_ns.load();
        if (advanced == kNever) continue;
        if (now - advanced >= slot->effective.budget_ns) {
          // Classified after its budget: stale like any expired sample.
          if (slot->state == StreamLivenessStateDTO::kAbsent) self->MarkStaleLocked(*slot);
          continue;
        }
        if (slot->state != StreamLivenessStateDTO::kLive) {
          slot->state = StreamLivenessStateDTO::kLive;
          slot->absent_classified = false;
          self->dirty.insert(slot->component);
        }
        slot->live.store(true);
        self->PushDeadlineLocked(slot, advanced + slot->effective.budget_ns);
      }
      self->fresh.clear();
      // Due deadlines only: each entry is refreshed from the slot's atomics.
      while (!self->deadlines.empty() && self->deadlines.top().at_ns <= now) {
        const auto entry = self->deadlines.top();
        self->deadlines.pop();
        const auto slot = entry.slot.lock();
        if (!slot || slot->serial != entry.serial || !self->CurrentLocked(slot)) continue;
        const auto budget = slot->effective.budget_ns;
        if (slot->state == StreamLivenessStateDTO::kAbsent) {
          // Start-up grace ended without a sample (a sample notifies instead).
          if (slot->last_advance_ns.load() == kNever && !slot->absent_classified &&
              now - slot->declared_at_ns >= budget) {
            slot->absent_classified = true;
            self->dirty.insert(slot->component);
          }
          continue;
        }
        if (slot->state != StreamLivenessStateDTO::kLive) continue;
        // A sample exactly one budget old is expired, so a re-queued deadline is always > now.
        auto advanced = slot->last_advance_ns.load();
        if (now - advanced < budget) {
          self->PushDeadlineLocked(slot, advanced + budget);
          continue;
        }
        slot->live.store(false);
        // A sample racing with the flag: it found `live` still set.
        advanced = slot->last_advance_ns.load();
        if (now - advanced < budget) {
          slot->live.store(true);
          self->PushDeadlineLocked(slot, advanced + budget);
          continue;
        }
        self->MarkStaleLocked(*slot);
      }
      for (const auto& component : self->dirty) {
        if (const auto watch = self->watches.find(component); watch != self->watches.end())
          watch->second->value.store(self->StaleFaultLocked(component), std::memory_order_release);
        auto verdict = self->VerdictLocked(component);
        const auto published = self->published.find(component);
        const bool changed = published == self->published.end() ? !verdict.empty()
                                                                 : published->second != verdict;
        if (!changed) continue;
        if (verdict.empty()) {
          self->published.erase(component);
        } else {
          self->published.insert_or_assign(component, verdict);
        }
        changes.emplace_back(component, std::move(verdict));
      }
      self->dirty.clear();
      // One timer for the earliest deadline.
      if (self->timer && self->timer_at_ns <= now) self->timer.reset();
      if (!self->deadlines.empty()) {
        const auto next = self->deadlines.top().at_ns;
        if (!self->timer || self->timer_at_ns != next) {
          if (self->timer) self->scheduler.cancel(*self->timer);
          std::weak_ptr<Shared> weak = self;
          self->timer_at_ns = next;
          self->timer = self->scheduler.schedule(
              next,
              [weak] {
                if (const auto shared = weak.lock()) Evaluate(shared);
              },
              SchedulerPriority::Monitoring);
        }
      }
    }
    if (changes.empty()) return;
    std::lock_guard log_lock(self->log_mutex);
    if (self->detached.load()) return;
    for (const auto& [component, verdict] : changes) {
      if (self->log) {
        try {
          std::string line = "INFO manyforge component stream health: component=" + component.value();
          const auto& shown = verdict.fault_loss;
          line += shown ? " condition=" + shown->message : std::string(" condition=none");
          self->log(line + "\n");
        } catch (...) {
        }
      }
    }
  }
};

StreamRecorder::StreamRecorder(std::shared_ptr<Slot> slot) noexcept : slot_(std::move(slot)) {}

StreamRecorder::~StreamRecorder() {
  const auto owner = slot_->owner.lock();
  if (!owner) return;
  try {
    std::lock_guard lock(owner->mutex);
    const auto found = owner->slots.find({slot_->component, slot_->defaults.stream_id});
    if (found == owner->slots.end() || found->second != slot_) return;
    owner->slots.erase(found);
    owner->dirty.insert(slot_->component);
    owner->ScheduleEvaluationLocked(owner);
  } catch (...) {
  }
}

const ControllableComponentId& StreamRecorder::component() const noexcept { return slot_->component; }
const std::string& StreamRecorder::stream_id() const noexcept { return slot_->defaults.stream_id; }

namespace {

void Advance(const std::shared_ptr<StreamRecorder::Slot>& slot) noexcept {
  slot->last_advance_ns.store(slot->clock->now());
  if (slot->live.load() || slot->notify_pending.exchange(true)) return;
  // Transition only: the registry classifies it on the Monitoring lane.
  const auto owner = slot->owner.lock();
  if (!owner) return;
  try {
    std::lock_guard lock(owner->mutex);
    if (owner->detached.load()) return;
    owner->fresh.push_back(slot);
    owner->ScheduleEvaluationLocked(owner);
  } catch (...) {
    slot->notify_pending.store(false);
  }
}

void RecordSample(const std::shared_ptr<StreamRecorder::Slot>& slot,
                  const std::optional<std::int64_t> source_stamp_ns) noexcept {
  if (source_stamp_ns &&
      slot->defaults.timestamp_source == StreamTimestampSourceDTO::kMessageStamp) {
    // Any change advances (a simulator reset steps backwards); an equal stamp does not.
    if (slot->last_stamp.exchange(*source_stamp_ns, std::memory_order_relaxed) == *source_stamp_ns)
      return;
  }
  Advance(slot);
}

}  // namespace

void StreamRecorder::Record(const std::int64_t source_stamp_ns) noexcept {
  RecordSample(slot_, source_stamp_ns);
}

void StreamRecorder::RecordArrival() noexcept { RecordSample(slot_, std::nullopt); }

ComponentStreamHealth::ComponentStreamHealth(std::shared_ptr<IClock> clock, IScheduler& scheduler)
    : shared_(std::make_shared<Shared>(std::move(clock), scheduler)) {
  if (!shared_->clock) throw std::invalid_argument("component stream health requires a clock");
}

ComponentStreamHealth::~ComponentStreamHealth() {
  DetachScheduler();
}

void ComponentStreamHealth::DetachScheduler() noexcept {
  static_cast<void>(DetachSchedulerUntil(std::chrono::steady_clock::time_point::max()));
}

bool ComponentStreamHealth::DetachSchedulerUntil(
    std::chrono::steady_clock::time_point deadline) noexcept {
  std::lock_guard detach_lock(shared_->detach_mutex);
  std::optional<ScheduleHandle> handle;
  {
    std::lock_guard lock(shared_->mutex);
    shared_->detached.store(true);
    handle = shared_->timer;
  }
  if (handle) {
    try { shared_->scheduler.cancel(*handle); } catch (...) { return false; }
    std::lock_guard lock(shared_->mutex);
    shared_->timer.reset();
  }
  (void)deadline;
  return true;
}

Result<std::unique_ptr<StreamRecorder>> ComponentStreamHealth::Declare(
    const ControllableComponentId& component, StreamDeclarationDTO defaults) {
  if (component.empty() || !defaults.locally_valid()) {
    return Result<std::unique_ptr<StreamRecorder>>::Err(
        {"component_stream_declaration_invalid", "the stream declaration is invalid",
         {{"stream_id", Truncated(defaults.stream_id, kMaxStreamIdBytes)}}});
  }
  auto slot = std::make_shared<StreamRecorder::Slot>(component);
  slot->defaults = std::move(defaults);
  slot->clock = shared_->clock;
  slot->owner = shared_;
  std::unique_ptr<StreamRecorder> recorder(new StreamRecorder(slot));
  std::lock_guard lock(shared_->mutex);
  if (shared_->detached.load())
    return Result<std::unique_ptr<StreamRecorder>>::Err(
        {"component_stream_declaration_invalid", "the stream declaration is invalid",
         {{"reason", "stream_health_detached"}}});
  slot->serial = shared_->next_serial++;
  slot->declared_at_ns = shared_->clock->now();
  shared_->ApplyEffectiveLocked(*slot);
  shared_->slots.insert_or_assign({component, slot->defaults.stream_id}, slot);
  shared_->PushDeadlineLocked(slot, slot->declared_at_ns + slot->effective.budget_ns);
  shared_->dirty.insert(component);
  shared_->ScheduleEvaluationLocked(shared_);
  return Result<std::unique_ptr<StreamRecorder>>::Ok(std::move(recorder));
}

bool ComponentStreamHealth::RecordDeclared(const ControllableComponentId& component,
                                           const std::string_view stream_id,
                                           const std::optional<std::int64_t> source_stamp_ns) {
  std::shared_ptr<StreamRecorder::Slot> slot;
  {
    std::lock_guard lock(shared_->mutex);
    const auto found = shared_->slots.find({component, std::string(stream_id)});
    if (found == shared_->slots.end()) return false;
    slot = found->second;
  }
  RecordSample(slot, source_stamp_ns);
  return true;
}

Result<void> ComponentStreamHealth::SetOverrides(std::vector<StreamParameterOverrideDTO> overrides) {
  std::map<SlotKey, StreamParameterOverrideDTO> table;
  for (auto& row : overrides) {
    if (!row.locally_valid() ||
        !table.emplace(SlotKey{row.controllable_component_id, row.stream_id}, row).second) {
      return Result<void>::Err({"component_stream_override_invalid",
                                "a component stream override is invalid or repeated",
                                {{"stream_id", Truncated(row.stream_id, kMaxStreamIdBytes)}}});
    }
  }
  std::lock_guard lock(shared_->mutex);
  if (shared_->detached.load())
    return Result<void>::Err({"component_stream_override_invalid",
                              "component stream health is detached", {}});
  if (table == shared_->overrides) return Result<void>::Ok();
  shared_->overrides = std::move(table);
  const auto now = shared_->clock->now();
  for (const auto& [key, slot] : shared_->slots) {
    const auto before = slot->effective;
    shared_->ApplyEffectiveLocked(*slot);
    if (before == slot->effective) continue;
    // A changed budget takes effect at once; older entries are refreshed lazily.
    const auto advanced = slot->last_advance_ns.load();
    shared_->PushDeadlineLocked(
        slot, std::max(now, (advanced == kNever ? slot->declared_at_ns : advanced) +
                                slot->effective.budget_ns));
    shared_->dirty.insert(key.first);
  }
  shared_->ScheduleEvaluationLocked(shared_);
  return Result<void>::Ok();
}

std::vector<StreamHealthDTO> ComponentStreamHealth::Read(const ControllableComponentId& component) const {
  std::vector<StreamHealthDTO> result;
  std::lock_guard lock(shared_->mutex);
  const auto now = shared_->clock->now();
  for (auto it = shared_->slots.lower_bound({component, std::string()});
       it != shared_->slots.end() && it->first.first == component; ++it) {
    const auto& slot = *it->second;
    StreamHealthDTO row;
    row.stream_id = slot.defaults.stream_id;
    row.state = slot.state;
    const auto advanced = slot.last_advance_ns.load();
    if (advanced != kNever) row.age_ns = std::max<std::int64_t>(0, now - advanced);
    row.budget_ns = slot.effective.budget_ns;
    row.severity = slot.effective.severity;
    row.timestamp_source = slot.defaults.timestamp_source;
    if (const auto code = Shared::ConditionCode(slot)) row.error = Shared::Condition(slot, *code);
    result.push_back(std::move(row));
  }
  return result;
}

std::shared_ptr<const ComponentStreamStaleFlag> ComponentStreamHealth::Watch(
    const ControllableComponentId& component) {
  std::lock_guard lock(shared_->mutex);
  auto& flag = shared_->watches[component];
  if (!flag) {
    flag = std::make_shared<ComponentStreamStaleFlag>();
    flag->value.store(shared_->StaleFaultLocked(component), std::memory_order_release);
  }
  return flag;
}

void ComponentStreamHealth::SetLog(std::function<void(const std::string&)> log) {
  std::lock_guard log_lock(shared_->log_mutex);
  shared_->log = std::move(log);
}

void ComponentStreamHealth::Evaluate() { Shared::Evaluate(shared_); }

}  // namespace manyforge::core
