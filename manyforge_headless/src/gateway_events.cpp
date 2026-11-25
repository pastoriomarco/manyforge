// The execution host's event-stream follower, its mirrors, the subscriptions
// and the run wake set (EXECUTION_PATH plan S1/S2, OD-380). Ports
// `manyforge_headless_api/execution_event_stream.py`,
// `manyforge_headless_api/private_stream.py` (client side) and the pushed
// tables of `manyforge_behavior/kernel_client.py`.

#include "manyforge_headless/gateway_events.hpp"

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace manyforge::headless::gateway {
namespace {

using Clock = std::chrono::steady_clock;
using content_identity::canonical_json;
using content_identity::CanonicalJsonParseLimits;
using content_identity::kMaximumCanonicalJsonDepth;
using content_identity::parse_canonical_json;

constexpr std::string_view kInvalid = "execution_event_stream_invalid";
constexpr std::string_view kUnavailable = "execution_owner_unavailable";
constexpr double kMaximumSafe = 9007199254740991.0;

/// `_counter`: an integer in [0, 2^53 - 1].
std::optional<std::uint64_t> Counter(const CanonicalValue *value) {
  if (value == nullptr || !value->is_number()) return std::nullopt;
  const double number = value->as_number();
  if (number < 0.0 || number > kMaximumSafe || std::floor(number) != number)
    return std::nullopt;
  return static_cast<std::uint64_t>(number);
}

const CanonicalValue *Member(const CanonicalValue &object,
                             std::string_view name) {
  return object.is_object() ? object.find_object_member(name) : nullptr;
}

/// The Python port's `KeyError`/`TypeError`: the member must exist.
const CanonicalValue &Required(const CanonicalValue &object,
                               std::string_view name,
                               std::string_view what) {
  const auto *member = Member(object, name);
  if (member == nullptr) throw std::invalid_argument(std::string(what));
  return *member;
}

class Fd {
 public:
  explicit Fd(int value = -1) noexcept : value_(value) {}
  ~Fd() {
    if (value_ >= 0) ::close(value_);
  }
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  [[nodiscard]] int get() const noexcept { return value_; }

 private:
  int value_;
};

/// A failure text as a canonical string (a system message is not
/// necessarily NFC UTF-8).
std::string Text(std::string text) {
  if (content_identity::is_utf8_nfc(text)) return text;
  return "event stream failed (the cause is not printable text)";
}

int PollMilliseconds(Clock::duration remaining) {
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
  return static_cast<int>(std::clamp<long long>(ms, 0, 1000LL * 60 * 60));
}

}  // namespace

// ---------------------------------------------------------------------------
// Message validation (decode_event_stream_message).
// ---------------------------------------------------------------------------

CanonicalValue DecodeEventStreamMessage(std::string_view payload) {
  CanonicalValue message;
  try {
    message = parse_canonical_json(
        payload, CanonicalJsonParseLimits{kMaximumEventStreamMessageBytes,
                                          kMaximumCanonicalJsonDepth,
                                          content_identity::kMaximumCanonicalJsonNodes});
  } catch (const std::exception &) {
    throw GatewayError(std::string(kInvalid), "event stream message is not JSON");
  }
  if (!message.is_object())
    throw GatewayError(std::string(kInvalid),
                       "event stream message root must be an object");
  const auto *api = message.find_object_member("api_version");
  if (api == nullptr || !api->is_string() ||
      api->as_string() != kEventStreamApiVersion)
    throw GatewayError(std::string(kInvalid),
                       "event stream message has another version");
  const auto *kind_value = message.find_object_member("type");
  const std::string kind =
      kind_value != nullptr && kind_value->is_string() ? kind_value->as_string()
                                                       : std::string();
  if (kind == "error") {
    const auto *error = message.find_object_member("error");
    const auto *code = error != nullptr ? Member(*error, "code") : nullptr;
    const auto *text = error != nullptr ? Member(*error, "message") : nullptr;
    throw GatewayError(
        code != nullptr && code->is_string() && !code->as_string().empty()
            ? code->as_string()
            : std::string("execution_event_stream_failed"),
        text != nullptr && text->is_string() ? text->as_string()
                                             : std::string("event stream ended"));
  }
  const auto member_is = [&](std::string_view name, auto predicate) {
    const auto *member = message.find_object_member(name);
    return member != nullptr && predicate(*member);
  };
  const bool valid =
      (kind == "snapshot" || kind == "event" || kind == "state" ||
       kind == "progress" || kind == "heartbeat") &&
      !((kind == "snapshot" || kind == "heartbeat") &&
        !Counter(message.find_object_member("cursor"))) &&
      !((kind == "snapshot" || kind == "state") &&
        !member_is("state", [](const auto &v) { return v.is_object(); })) &&
      !(kind == "progress" &&
        !member_is("operation_progress", [](const auto &v) { return v.is_array(); })) &&
      !(kind == "event" &&
        !member_is("event", [](const auto &v) {
          return v.is_object() && Counter(v.find_object_member("sequence"));
        }));
  if (!valid)
    throw GatewayError(std::string(kInvalid),
                       "event stream message has an invalid envelope");
  return message;
}

// ---------------------------------------------------------------------------
// WakeSet.
// ---------------------------------------------------------------------------

void WakeSet::WatchOperation(RunId run_id, std::uint64_t operation_id) {
  std::lock_guard lock(mutex_);
  operations_[operation_id].insert(run_id);
  by_run_[run_id].first.insert(operation_id);
}

void WakeSet::WatchPlanningJob(RunId run_id, std::string job_id) {
  std::lock_guard lock(mutex_);
  planning_jobs_[job_id].insert(run_id);
  by_run_[run_id].second.insert(std::move(job_id));
}

void WakeSet::UnwatchOperation(RunId run_id, std::uint64_t operation_id) {
  std::lock_guard lock(mutex_);
  const auto found = by_run_.find(run_id);
  if (found == by_run_.end() || found->second.first.erase(operation_id) == 0) return;
  auto runs = operations_.find(operation_id);
  runs->second.erase(run_id);
  if (runs->second.empty()) operations_.erase(runs);
  if (found->second.first.empty() && found->second.second.empty()) by_run_.erase(found);
}

void WakeSet::UnwatchPlanningJob(RunId run_id, const std::string &job_id) {
  std::lock_guard lock(mutex_);
  const auto found = by_run_.find(run_id);
  if (found == by_run_.end() || found->second.second.erase(job_id) == 0) return;
  auto runs = planning_jobs_.find(job_id);
  runs->second.erase(run_id);
  if (runs->second.empty()) planning_jobs_.erase(runs);
  if (found->second.first.empty() && found->second.second.empty()) by_run_.erase(found);
}

void WakeSet::UnwatchRun(RunId run_id) {
  std::lock_guard lock(mutex_);
  const auto found = by_run_.find(run_id);
  if (found == by_run_.end()) return;
  for (const auto operation_id : found->second.first) {
    auto runs = operations_.find(operation_id);
    runs->second.erase(run_id);
    if (runs->second.empty()) operations_.erase(runs);
  }
  for (const auto &job_id : found->second.second) {
    auto runs = planning_jobs_.find(job_id);
    runs->second.erase(run_id);
    if (runs->second.empty()) planning_jobs_.erase(runs);
  }
  by_run_.erase(found);
}

void WakeSet::WakeLocked(RunId run_id) {
  if (woken_.insert(run_id).second) woken_changed_.notify_all();
}

void WakeSet::Wake(RunId run_id) {
  std::lock_guard lock(mutex_);
  WakeLocked(run_id);
}

std::vector<WakeSet::RunId> WakeSet::Wait(Clock::time_point deadline) {
  std::unique_lock lock(mutex_);
  woken_changed_.wait_until(lock, deadline, [this] { return !woken_.empty(); });
  std::vector<RunId> woken(woken_.begin(), woken_.end());
  woken_.clear();
  return woken;
}

void WakeSet::WakeOperations(const std::function<bool(std::uint64_t)> &changed) {
  std::lock_guard lock(mutex_);
  for (const auto &[operation_id, runs] : operations_)
    if (changed(operation_id))
      for (const auto run_id : runs) WakeLocked(run_id);
}

void WakeSet::WakePlanningJobs(
    const std::function<bool(const std::string &)> &changed) {
  std::lock_guard lock(mutex_);
  for (const auto &[job_id, runs] : planning_jobs_)
    if (changed(job_id))
      for (const auto run_id : runs) WakeLocked(run_id);
}

void WakeSet::WakeAllWatched() {
  std::lock_guard lock(mutex_);
  for (const auto &[run_id, _watches] : by_run_) WakeLocked(run_id);
}

// ---------------------------------------------------------------------------
// PushedOperationMirror (PushedOperationTable + PushedPlanningJobs).
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaximumPlanningJobs = 8U;

bool IsPlanningJobState(const std::string &state) {
  return state == "accepted" || state == "planning" || state == "done" ||
         state == "failed" || state == "cancelled";
}

void ReadOperations(const CanonicalValue &state, PushedOperationMirror::Table &table) {
  const auto &operations = Required(state, "operations", "operations");
  const auto change = Counter(&Required(operations, "change", "change"));
  const auto &resident = Required(operations, "resident", "resident");
  const auto total = Counter(&Required(operations, "resident_total", "resident_total"));
  if (!change || !resident.is_array() || !total)
    throw std::invalid_argument("operations table counters are invalid");
  if (resident.as_array().size() > *total)
    throw std::invalid_argument(
        "operations table lists more operations than it counts");
  for (const auto &entry : resident.as_array()) {
    const auto operation_id =
        Counter(&Required(entry, "operation_id", "operation_id"));
    const auto &operation = Required(entry, "operation", "operation");
    const auto &control_pending =
        Required(entry, "control_pending", "control_pending");
    const auto &last_control = Required(entry, "last_control", "last_control");
    const auto *own_id = Member(operation, "operation_id");
    if (!operation_id || *operation_id < 1 || table.entries.count(*operation_id) ||
        !control_pending.is_bool() ||
        !(last_control.is_null() || last_control.is_object()) ||
        !(operation.is_null() ||
          (operation.is_object() && own_id != nullptr && own_id->is_number() &&
           own_id->as_number() == static_cast<double>(*operation_id))))
      throw std::invalid_argument("operations table entry is invalid");
    table.entries.emplace(*operation_id, entry);
  }
  table.change = *change;
  const auto *active_tcp = state.find_object_member("active_tcp_baseline");
  table.active_tcp = active_tcp != nullptr ? *active_tcp : CanonicalValue(nullptr);
}

void ReadPlanningJobs(const CanonicalValue &state, PushedOperationMirror::Table &table) {
  const auto &rows = Required(state, "planning_jobs", "planning_jobs");
  if (!rows.is_array() || rows.as_array().size() > kMaximumPlanningJobs)
    throw std::invalid_argument("planning jobs are not a bounded list");
  for (const auto &row : rows.as_array()) {
    const auto &job_id = Required(row, "job_id", "job_id");
    const auto &kind = Required(row, "kind", "kind");
    const auto &job_state = Required(row, "state", "state");
    const auto &run = Required(row, "program_run_id", "program_run_id");
    const auto run_id = Counter(&run);
    if (!job_id.is_string() || job_id.as_string().empty() ||
        table.planning_jobs.count(job_id.as_string()) || !kind.is_string() ||
        (kind.as_string() != "pick" && kind.as_string() != "place") ||
        !job_state.is_string() || !IsPlanningJobState(job_state.as_string()) ||
        !(run.is_null() || (run_id && *run_id >= 1)))
      throw std::invalid_argument("planning job row is invalid");
    table.planning_jobs.emplace(job_id.as_string(), job_state.as_string());
  }
}

}  // namespace

std::pair<std::shared_ptr<const PushedOperationMirror::Table>,
          std::shared_ptr<const PushedOperationMirror::Table>>
PushedOperationMirror::ApplyState(const CanonicalValue &state) {
  auto table = std::make_shared<Table>();
  try {
    ReadOperations(state, *table);
  } catch (const std::invalid_argument &error) {
    StreamLost();
    throw GatewayError(std::string(kInvalid),
                       std::string("pushed operations table is invalid: ") +
                           error.what());
  }
  try {
    ReadPlanningJobs(state, *table);
  } catch (const std::invalid_argument &error) {
    StreamLost();
    throw GatewayError(std::string(kInvalid),
                       std::string("pushed planning jobs are invalid: ") +
                           error.what());
  }
  std::shared_ptr<const Table> after = std::move(table);
  std::lock_guard lock(mutex_);
  auto before = std::exchange(table_, after);
  return {std::move(before), std::move(after)};
}

std::shared_ptr<const PushedOperationMirror::Table> PushedOperationMirror::StreamLost() {
  std::lock_guard lock(mutex_);
  return std::exchange(table_, nullptr);
}

std::shared_ptr<const PushedOperationMirror::Table> PushedOperationMirror::Current() const {
  std::lock_guard lock(mutex_);
  return table_;
}

std::optional<CanonicalValue> PushedOperationMirror::Envelope(
    std::uint64_t operation_id, std::uint64_t after_change) const {
  const auto table = Current();
  if (table == nullptr)
    throw GatewayError(std::string(kEventMirrorUnavailable),
                       "pushed operations table is unavailable");
  if (table->change < after_change)
    throw GatewayError(std::string(kEventMirrorBehind),
                       "pushed operations table predates the operation's acceptance");
  const auto found = table->entries.find(operation_id);
  if (found == table->entries.end()) return std::nullopt;
  const auto &entry = found->second;
  const auto &operation = *entry.find_object_member("operation");
  if (operation.is_null()) return std::nullopt;
  return CanonicalValue(CanonicalValue::Object{
      {"ok", CanonicalValue(true)},
      {"command", CanonicalValue("read")},
      {"operation", operation},
      {"resident", CanonicalValue(true)},
      {"last_control", *entry.find_object_member("last_control")},
      {"control_pending", *entry.find_object_member("control_pending")},
      {"active_tcp", table->active_tcp},
  });
}

std::optional<std::string> PushedOperationMirror::PlanningJobState(
    std::string_view job_id) const {
  const auto table = Current();
  if (table == nullptr)
    throw GatewayError(std::string(kEventMirrorUnavailable),
                       "pushed planning jobs are unavailable");
  const auto found = table->planning_jobs.find(std::string(job_id));
  if (found == table->planning_jobs.end()) return std::nullopt;
  return found->second;
}

// ---------------------------------------------------------------------------
// FollowerSignal and EventSubscription.
// ---------------------------------------------------------------------------

FollowerSignal::FollowerSignal() : fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
  if (fd_ < 0)
    throw GatewayError(std::string(kUnavailable),
                       std::string("event follower signal failed: ") +
                           std::strerror(errno));
}

FollowerSignal::~FollowerSignal() { ::close(fd_); }

void FollowerSignal::Raise() noexcept {
  const std::uint64_t one = 1;
  // A non-blocking eventfd write fails only at the counter's maximum, which
  // still reads as raised.
  [[maybe_unused]] const auto written = ::write(fd_, &one, sizeof(one));
}

void FollowerSignal::Clear() noexcept {
  std::uint64_t count = 0;
  // Nothing to read (EAGAIN) is already clear.
  [[maybe_unused]] const auto read = ::read(fd_, &count, sizeof(count));
}

namespace {

/// The stream position a message carries (snapshot, heartbeat, event).
std::optional<std::uint64_t> MessageCursor(const CanonicalValue &message) {
  const auto &kind = message.find_object_member("type")->as_string();
  if (kind == "snapshot" || kind == "heartbeat")
    return Counter(message.find_object_member("cursor"));
  if (kind == "event")
    return Counter(message.find_object_member("event")->find_object_member("sequence"));
  return std::nullopt;
}

EventMessage Disconnected(const std::string &code, const std::string &text) {
  return std::make_shared<const CanonicalValue>(CanonicalValue::Object{
      {"type", CanonicalValue("disconnected")},
      {"code", CanonicalValue(Text(code))},
      {"message", CanonicalValue(Text(text))},
  });
}

}  // namespace

EventSubscription::EventSubscription(std::size_t capacity,
                                     std::shared_ptr<FollowerSignal> signal,
                                     std::uint64_t cursor)
    : capacity_(capacity),
      signal_(std::move(signal)),
      pushed_cursor_(cursor),
      taken_cursor_(cursor),
      applied_cursor_(cursor),
      feed_cursor_(cursor) {}

EventMessage EventSubscription::Next(std::chrono::nanoseconds timeout) {
  std::unique_lock lock(mutex_);
  // Asking again: the last message taken was applied.
  applied_cursor_ = taken_cursor_;
  if (std::exchange(last_taken_applies_, false)) resyncs_ = 0;
  changed_.wait_for(lock, timeout, [this] { return closed_.load() || !queue_.empty(); });
  if (closed_.load() || queue_.empty()) return nullptr;
  const bool was_full = queue_.size() >= capacity_;
  auto entry = std::move(queue_.front());
  queue_.pop_front();
  taken_cursor_ = entry.cursor;
  last_taken_applies_ =
      entry.message->find_object_member("type")->as_string() != "disconnected";
  lock.unlock();
  // An own feed waits for this room.
  if (was_full) signal_->Raise();
  return std::move(entry.message);
}

void EventSubscription::Close() {
  {
    std::lock_guard lock(mutex_);
    closed_.store(true);
    queue_.clear();
    changed_.notify_all();
  }
  // The follower drops this subscription's own connection at once.
  signal_->Raise();
}

void EventSubscription::Resync() {
  auto disconnected = Disconnected(
      std::string(kInvalid),
      "the subscriber could not apply a message; it reads again from a fresh snapshot");
  {
    std::lock_guard lock(mutex_);
    if (closed_.load()) return;
    // The last message taken was not applied.
    last_taken_applies_ = false;
    RestartLocked(std::move(disconnected), applied_cursor_, ++resyncs_);
  }
  signal_->Raise();
}

void EventSubscription::Detach(EventMessage disconnected) {
  std::lock_guard lock(mutex_);
  if (closed_.load()) return;
  RestartLocked(std::move(disconnected), taken_cursor_, 0);
}

void EventSubscription::RestartLocked(EventMessage disconnected,
                                      std::uint64_t after_cursor,
                                      std::uint32_t resyncs) {
  queue_.clear();
  queue_.push_back(Entry{std::move(disconnected), after_cursor});
  pushed_cursor_ = taken_cursor_ = feed_cursor_ = after_cursor;
  feed_resyncs_ = resyncs;
  epoch_.store(epoch_.load() + 1);
  changed_.notify_all();
}

EventSubscription::PushResult EventSubscription::Push(
    EventMessage message, std::optional<std::uint64_t> cursor, std::uint64_t epoch) {
  std::lock_guard lock(mutex_);
  if (closed_.load()) return PushResult::kPushed;
  if (epoch != epoch_.load()) return PushResult::kStale;
  if (queue_.size() >= capacity_) return PushResult::kFull;
  pushed_cursor_ = cursor.value_or(pushed_cursor_);
  queue_.push_back(Entry{std::move(message), pushed_cursor_});
  changed_.notify_all();
  return PushResult::kPushed;
}

EventSubscription::Feed EventSubscription::CurrentFeed() const {
  std::lock_guard lock(mutex_);
  return Feed{epoch_.load(), feed_cursor_, feed_resyncs_};
}

bool EventSubscription::HasRoom() const {
  std::lock_guard lock(mutex_);
  return closed_.load() || queue_.size() < capacity_;
}

// ---------------------------------------------------------------------------
// ExecutionEvents.
// ---------------------------------------------------------------------------

struct ExecutionEvents::Link {
  Link(std::uint64_t after_cursor, std::uint64_t feed_epoch,
       std::chrono::milliseconds initial_delay)
      : cursor(after_cursor), epoch(feed_epoch), delay(initial_delay) {}
  ~Link() { Disconnect(); }
  Link(Link &&other) noexcept
      : cursor(other.cursor),
        epoch(other.epoch),
        fd(std::exchange(other.fd, -1)),
        delay(other.delay),
        due(other.due) {}
  Link &operator=(Link &&other) noexcept {
    if (this != &other) {
      Disconnect();
      cursor = other.cursor;
      epoch = other.epoch;
      fd = std::exchange(other.fd, -1);
      delay = other.delay;
      due = other.due;
    }
    return *this;
  }

  [[nodiscard]] bool open() const noexcept { return fd >= 0; }
  void Disconnect() noexcept {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }
  /// Closed; the next connect after the current delay, which doubles.
  void Lost(std::chrono::milliseconds maximum) {
    Disconnect();
    due = Clock::now() + delay;
    delay = std::min(delay * 2, maximum);
  }
  /// A message arrived: the delay starts over, the dead-link clock too.
  void Delivered(const ExecutionEventsOptions &options) {
    delay = options.backoff_initial;
    due = Clock::now() + options.dead_link;
  }

  /// Events continue strictly after this cursor (the last one delivered).
  std::uint64_t cursor;
  /// The subscription feed this link serves (0: the shared connection).
  std::uint64_t epoch;
  int fd{-1};
  std::chrono::milliseconds delay;
  /// Open: the dead-link deadline; closed: the next connect.
  Clock::time_point due{};
};

struct ExecutionEvents::OwnFeed {
  std::shared_ptr<EventSubscription> subscription;
  Link link;
};

ExecutionEvents::ExecutionEvents(std::filesystem::path execution_run_root,
                                 std::shared_ptr<WakeSet> wake_set,
                                 ExecutionEventsOptions options)
    : root_(std::move(execution_run_root)),
      wake_set_(std::move(wake_set)),
      options_(options),
      signal_(std::make_shared<FollowerSignal>()),
      stop_fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
  if (stop_fd_ < 0)
    throw GatewayError(std::string(kUnavailable),
                       std::string("event follower stop signal failed: ") +
                           std::strerror(errno));
}

ExecutionEvents::~ExecutionEvents() {
  Close();
  ::close(stop_fd_);
}

void ExecutionEvents::Start() {
  std::lock_guard lock(lifecycle_mutex_);
  if (started_ || stopping_.load()) return;
  started_ = true;
  follower_ = std::thread([this] {
    static_cast<void>(::pthread_setname_np(::pthread_self(), "mf_ev_follower"));
    Run();
  });
}

void ExecutionEvents::Close() {
  std::thread follower;
  {
    std::lock_guard lock(lifecycle_mutex_);
    if (!stopping_.exchange(true)) {
      const std::uint64_t one = 1;
      // An eventfd write of 1 cannot fail short of a closed descriptor.
      [[maybe_unused]] const auto written = ::write(stop_fd_, &one, sizeof(one));
    }
    follower.swap(follower_);
  }
  // Joined with no lock held; the follower sees the eventfd at once.
  if (follower.joinable()) follower.join();
  std::vector<std::shared_ptr<EventSubscription>> subscribers;
  {
    std::lock_guard lock(subscribers_mutex_);
    subscribers.swap(subscribers_);
  }
  for (const auto &subscriber : subscribers) subscriber->Close();
}

std::shared_ptr<EventSubscription> ExecutionEvents::Subscribe() {
  auto subscriber = std::make_shared<EventSubscription>(
      options_.subscription_capacity, signal_, cursor_.load());
  {
    std::lock_guard lock(subscribers_mutex_);
    if (!stopping_.load()) {
      subscribers_.push_back(subscriber);
      return subscriber;
    }
  }
  subscriber->Close();
  return subscriber;
}

bool ExecutionEvents::Connected() const noexcept { return connected_.load(); }

// `closed()` and `epoch()` are atomics: no lock nests in the list's.
std::vector<std::shared_ptr<EventSubscription>> ExecutionEvents::Subscribers() {
  std::vector<std::shared_ptr<EventSubscription>> shared;
  std::lock_guard lock(subscribers_mutex_);
  for (const auto &subscriber : subscribers_)
    if (subscriber->epoch() == 0) shared.push_back(subscriber);
  return shared;
}

void ExecutionEvents::Rescan(std::vector<OwnFeed> &own) {
  std::vector<std::shared_ptr<EventSubscription>> detached;
  {
    std::lock_guard lock(subscribers_mutex_);
    std::erase_if(subscribers_, [](const auto &subscriber) { return subscriber->closed(); });
    for (const auto &subscriber : subscribers_)
      if (subscriber->epoch() != 0) detached.push_back(subscriber);
  }
  std::erase_if(own, [](const OwnFeed &feed) { return feed.subscription->closed(); });
  for (auto &subscriber : detached)
    if (std::none_of(own.begin(), own.end(), [&](const OwnFeed &feed) {
          return feed.subscription == subscriber;
        }))
      own.push_back(OwnFeed{std::move(subscriber), Link(0, 0, options_.backoff_initial)});
  const auto now = Clock::now();
  for (auto &feed : own) {
    const auto current = feed.subscription->CurrentFeed();
    if (current.epoch == feed.link.epoch) continue;
    feed.link = Link(current.after_cursor, current.epoch, options_.backoff_initial);
    if (current.resyncs == 0) continue;
    // The consumer could not apply a message: its feed opens after the
    // current delay of its consecutive resyncs (0.1 s doubling to 2 s), with
    // the poll timeout as the wait.
    auto delay = options_.backoff_initial;
    for (std::uint32_t step = 1; step < current.resyncs && delay < options_.backoff_maximum;
         ++step)
      delay *= 2;
    delay = std::min(delay, options_.backoff_maximum);
    feed.link.due = now + delay;
    feed.link.delay = std::min(delay * 2, options_.backoff_maximum);
  }
}

void ExecutionEvents::Run() {
  receive_buffer_.resize(kMaximumEventStreamMessageBytes);
  Link shared(0, 0, options_.backoff_initial);
  std::vector<OwnFeed> own;
  std::vector<OwnFeed *> polled;
  std::vector<pollfd> watched;
  const LinkFailure dead{"execution_event_stream_dead",
                         "event stream sent no message or heartbeat in time"};
  while (!stopping_.load()) {
    signal_->Clear();
    Rescan(own);
    auto now = Clock::now();
    if (!shared.open() && now >= shared.due)
      if (auto failure = Open(shared)) SharedLost(shared, *failure);
    auto wake_at = shared.due;
    polled.clear();
    for (auto &feed : own) {
      // An own feed connects and reads only while its subscription has room;
      // its dead-link clock waits with it.
      if (!feed.subscription->HasRoom()) {
        if (feed.link.open()) feed.link.due = now + options_.dead_link;
        continue;
      }
      if (!feed.link.open() && now >= feed.link.due)
        if (auto failure = Open(feed.link)) OwnLost(feed, *failure);
      wake_at = std::min(wake_at, feed.link.due);
      if (feed.link.open()) polled.push_back(&feed);
    }
    watched.assign({pollfd{stop_fd_, POLLIN, 0}, pollfd{signal_->fd(), POLLIN, 0}});
    const bool shared_polled = shared.open();
    if (shared_polled) watched.push_back(pollfd{shared.fd, POLLIN, 0});
    for (const auto *feed : polled) watched.push_back(pollfd{feed->link.fd, POLLIN, 0});
    if (::poll(watched.data(), watched.size(), PollMilliseconds(wake_at - Clock::now())) < 0)
      continue;  // EINTR.
    if (watched[0].revents != 0) return;
    now = Clock::now();
    std::size_t index = 2;
    if (shared_polled) {
      if (watched[index++].revents != 0) {
        EventMessage message;
        auto failure = Receive(shared, message);
        if (!failure && message != nullptr) {
          try {
            Deliver(shared, message);
          } catch (const GatewayError &error) {
            failure = LinkFailure{error.code(), error.what()};
          }
        }
        if (failure) SharedLost(shared, *failure);
      } else if (now >= shared.due) {
        SharedLost(shared, dead);
      }
    }
    for (auto *feed : polled) {
      if (watched[index++].revents != 0) {
        EventMessage message;
        if (auto failure = Receive(feed->link, message))
          OwnLost(*feed, *failure);
        else if (message != nullptr)
          FeedOwn(*feed, message);
      } else if (now >= feed->link.due) {
        OwnLost(*feed, dead);
      }
    }
  }
}

std::optional<ExecutionEvents::LinkFailure> ExecutionEvents::Open(Link &link) {
  try {
    link.fd = root_.Connect(kExecutionEventsSocketFile, options_.connect_limit,
                            static_cast<int>(kMaximumEventStreamMessageBytes));
  } catch (const std::exception &error) {
    return LinkFailure{std::string(kUnavailable),
                       std::string("event stream connect failed: ") + error.what()};
  }
  const auto request = canonical_json(CanonicalValue(CanonicalValue::Object{
      {"after_cursor", CanonicalValue(static_cast<double>(link.cursor))},
      {"api_version", CanonicalValue("manyforge.execution_command.v4")},
      {"command", CanonicalValue("subscribe_events")},
  }));
  ssize_t sent = -1;
  do {
    sent = ::send(link.fd, request.data(), request.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (sent < 0 && errno == EINTR);
  if (sent < 0)
    return LinkFailure{std::string(kUnavailable),
                       std::string("event stream connect failed: ") +
                           std::strerror(errno)};
  if (static_cast<std::size_t>(sent) != request.size())
    return LinkFailure{"execution_command_transport_failure",
                       "subscribe packet was truncated"};
  link.due = Clock::now() + options_.dead_link;
  return std::nullopt;
}

std::optional<ExecutionEvents::LinkFailure> ExecutionEvents::Receive(
    Link &link, EventMessage &message) {
  // Receive (PrivateStreamSubscription.receive); poll found the link readable.
  iovec buffer{receive_buffer_.data(), receive_buffer_.size()};
  msghdr header{};
  header.msg_iov = &buffer;
  header.msg_iovlen = 1;
  ssize_t count = -1;
  do {
    count = ::recvmsg(link.fd, &header, MSG_DONTWAIT);
  } while (count < 0 && errno == EINTR);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return std::nullopt;
  if (count < 0)
    return LinkFailure{std::string(kUnavailable),
                       std::string("event stream failed: ") + std::strerror(errno)};
  if (count == 0) return LinkFailure{std::string(kUnavailable), "event stream closed"};
  if ((header.msg_flags & MSG_TRUNC) != 0)
    return LinkFailure{std::string(kInvalid), "event stream message is oversized"};
  try {
    message = std::make_shared<const CanonicalValue>(DecodeEventStreamMessage(
        std::string_view(receive_buffer_.data(), static_cast<std::size_t>(count))));
  } catch (const GatewayError &error) {
    return LinkFailure{error.code(), error.what()};
  }
  return std::nullopt;
}

void ExecutionEvents::Deliver(Link &link, const EventMessage &message) {
  const auto &kind = message->find_object_member("type")->as_string();
  if (kind == "snapshot" || kind == "state") {
    auto [before, after] =
        operations_.ApplyState(*message->find_object_member("state"));
    if (kind == "snapshot") connected_.store(true);
    if (wake_set_ != nullptr) {
      if (before == nullptr) {
        wake_set_->WakeAllWatched();
      } else {
        const bool active_tcp_changed = !(before->active_tcp == after->active_tcp);
        wake_set_->WakeOperations([&](std::uint64_t operation_id) {
          const auto was = before->entries.find(operation_id);
          const auto now = after->entries.find(operation_id);
          const bool had = was != before->entries.end();
          const bool has = now != after->entries.end();
          if (had != has) return true;
          // Absent from both: it may have retired in between.
          if (!has) return before->change != after->change;
          return active_tcp_changed || !(was->second == now->second);
        });
        wake_set_->WakePlanningJobs([&](const std::string &job_id) {
          const auto was = before->planning_jobs.find(job_id);
          const auto now = after->planning_jobs.find(job_id);
          const bool had = was != before->planning_jobs.end();
          const bool has = now != after->planning_jobs.end();
          return had != has || (has && was->second != now->second);
        });
      }
    }
  }
  // `progress` rows wake no run: the runner reads no progress.
  const auto cursor = MessageCursor(*message);
  EventMessage overflow;
  for (const auto &subscriber : Subscribers())
    if (subscriber->Push(message, cursor, 0) == EventSubscription::PushResult::kFull) {
      // The shared connection never waits for one consumer.
      if (overflow == nullptr)
        overflow = Disconnected(
            std::string(kEventSubscriptionOverflow),
            "this subscriber fell behind; it resumes on its own connection");
      subscriber->Detach(overflow);
    }
  if (overflow != nullptr) signal_->Raise();
  // Advance only after the message was applied.
  if (cursor) {
    link.cursor = *cursor;
    cursor_.store(*cursor);
  }
  link.Delivered(options_);
}

void ExecutionEvents::FeedOwn(OwnFeed &feed, const EventMessage &message) {
  const auto cursor = MessageCursor(*message);
  if (feed.subscription->Push(message, cursor, feed.link.epoch) !=
      EventSubscription::PushResult::kPushed) {
    // A resync moved the subscription to a newer feed (this feed is read only
    // with room, so it is not full): the next rescan starts that one.
    feed.link.Disconnect();
    return;
  }
  if (cursor) feed.link.cursor = *cursor;
  feed.link.Delivered(options_);
}

void ExecutionEvents::SharedLost(Link &link, const LinkFailure &failure) {
  link.Lost(options_.backoff_maximum);
  // Unavailable, never stale, until the next snapshot; the watchers wake when
  // it stops being available (also after a malformed table cleared it).
  operations_.StreamLost();
  if (connected_.exchange(false) && wake_set_ != nullptr) wake_set_->WakeAllWatched();
  const auto message = Disconnected(failure.code, failure.message);
  bool detached = false;
  for (const auto &subscriber : Subscribers())
    if (subscriber->Push(message, std::nullopt, 0) == EventSubscription::PushResult::kFull) {
      subscriber->Detach(message);
      detached = true;
    }
  if (detached) signal_->Raise();
}

void ExecutionEvents::OwnLost(OwnFeed &feed, const LinkFailure &failure) {
  feed.link.Lost(options_.backoff_maximum);
  // There is room: an own feed connects and reads only with room. A resync
  // in between makes it stale, and its own `disconnected` stands.
  static_cast<void>(feed.subscription->Push(Disconnected(failure.code, failure.message),
                                            std::nullopt, feed.link.epoch));
}

}  // namespace manyforge::headless::gateway
