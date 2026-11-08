// Short, opt-in real-storage measurements. No runtime owner or robot process.
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_headless/event_historian.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace manyforge::headless;
using manyforge::content_identity::canonical_json;
using manyforge::content_identity::CanonicalValue;
using Clock = std::chrono::steady_clock;

struct Scratch final {
  std::filesystem::path path;
  Scratch() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "manyforge-history-perf-XXXXXX").string();
    const auto created = ::mkdtemp(pattern.data());
    if (created == nullptr) throw std::runtime_error("mkdtemp failed");
    path = created;
  }
  ~Scratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

struct Measurements final {
  std::vector<double> ms;
  template <class Function>
  auto measure(Function function) {
    const auto start = Clock::now();
    auto result = function();
    ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    return result;
  }
  CanonicalValue value() {
    std::sort(ms.begin(), ms.end());
    return CanonicalValue(
        CanonicalValue::Object{{"samples", CanonicalValue(static_cast<double>(ms.size()))},
                               {"p95_ms", CanonicalValue(ms.at((ms.size() * 95 - 1) / 100))},
                               {"p99_ms", CanonicalValue(ms.at((ms.size() * 99 - 1) / 100))},
                               {"max_ms", CanonicalValue(ms.back())}});
  }
};

CanonicalValue Payload(const std::string& profile) {
  CanonicalValue data("small");
  if (profile == "text") data = CanonicalValue(std::string(260000, 'x'));
  if (profile == "tree") data = CanonicalValue(CanonicalValue::Array(120000, CanonicalValue(0.0)));
  return CanonicalValue(
      CanonicalValue::Object{{"clock_owner", CanonicalValue("history-perf.1")},
                             {"message", CanonicalValue("qualification error")},
                             {"program_run_id", CanonicalValue("run.history-perf")},
                             {"value", std::move(data)}});
}

std::int64_t Count(const CanonicalValue& object, std::string_view name) {
  return manyforge::content_identity::parse_nonnegative_int64(*object.find_object_member(name));
}

/// Failure-capture soak invariant: every written row is exactly one of
/// retained-unprotected, capture-protected or pruned, and the capture budget
/// and metadata never exceed their limits.
void RequireCaptureInvariants(const CanonicalValue& retention, std::size_t written) {
  const auto& partitions = retention.find_object_member("partitions")->as_array();
  const auto& captures = *retention.find_object_member("captures");
  std::int64_t accounted = Count(captures, "protected_count");
  for (const auto& partition : partitions) {
    accounted += Count(partition, "retained_count") + Count(partition, "pruned_count");
  }
  if (accounted != static_cast<std::int64_t>(written))
    throw std::runtime_error("capture soak accounting differs from the written stream: " +
                             std::to_string(accounted) + " != " + std::to_string(written));
  if (Count(captures, "live_count") > Count(captures, "live_capture_limit") ||
      Count(captures, "protected_bytes") > Count(captures, "protected_byte_limit") ||
      Count(captures, "open_count") > 1)
    throw std::runtime_error("capture budget or metadata exceeded its bound");
}

bool BothPartitionsPruned(const CanonicalValue& page) {
  const auto& partitions =
      page.find_object_member("retention")->find_object_member("partitions")->as_array();
  return partitions.size() == 2 &&
         std::all_of(partitions.begin(), partitions.end(), [](const auto& partition) {
           return manyforge::content_identity::parse_nonnegative_int64(
                      *partition.find_object_member("pruned_count")) >= 32;
         });
}
double ThreadCpuMs() {
  timespec now{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<double>(now.tv_sec) * 1e3 + static_cast<double>(now.tv_nsec) / 1e6;
}

CanonicalValue Percentiles(std::vector<double> values, const std::string_view unit) {
  if (values.empty()) return CanonicalValue(nullptr);
  std::sort(values.begin(), values.end());
  const auto at = [&](const std::size_t percent) {
    return CanonicalValue(values.at((values.size() * percent - 1) / 100));
  };
  return CanonicalValue(CanonicalValue::Object{
      {"samples", CanonicalValue(static_cast<double>(values.size()))},
      {"unit", CanonicalValue(std::string(unit))},
      {"p50", at(50)}, {"p99", at(99)}, {"max", CanonicalValue(values.back())}});
}

/// Forwards to the real store and measures each writer transaction on the
/// writer thread: batch size, CPU, wall time and enqueue-to-commit latency.
class MeasuringStore final : public IEventHistoryStore {
public:
  MeasuringStore(std::shared_ptr<SqliteEventHistoryStore> store, std::filesystem::path wal)
      : store_(std::move(store)), wal_(std::move(wal)) {}

  void ObserveDeliveryLoss(const std::uint64_t losses) override { store_->ObserveDeliveryLoss(losses); }
  AppendStatus Append(const HistorianEvent& event) override { return store_->Append(event); }
  HistorianPage Query(const HistorianQuery& query) override { return store_->Query(query); }
  std::optional<HistorianStoredEvent> Read(const std::uint64_t sequence) override {
    return store_->Read(sequence);
  }

  AppendBatchResult AppendBatch(const std::span<const HistorianEvent> events) override {
    if (writer_nice_ == 1000) {
      writer_nice_ = getpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)));
      writer_policy_ = sched_getscheduler(0);
    }
    const auto cpu = ThreadCpuMs();
    const auto start = Clock::now();
    auto result = store_->AppendBatch(events);
    const auto done = Clock::now();
    std::lock_guard lock(mutex_);
    batches_.push_back(static_cast<double>(events.size()));
    batch_wall_ms_.push_back(std::chrono::duration<double, std::milli>(done - start).count());
    writer_cpu_ms_ += ThreadCpuMs() - cpu;
    for (std::size_t index = 0; index < result.statuses.size(); ++index) {
      const auto found = enqueued_.find(events[index].event_id);
      if (found == enqueued_.end()) continue;
      commit_ms_.push_back(std::chrono::duration<double, std::milli>(done - found->second).count());
      enqueued_.erase(found);
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(wal_, error);
    if (!error) wal_max_bytes_ = std::max<double>(wal_max_bytes_, static_cast<double>(size));
    return result;
  }

  void Enqueued(const std::string& event_id, const Clock::time_point at) {
    std::lock_guard lock(mutex_);
    enqueued_.emplace(event_id, at);
  }

  CanonicalValue::Object Take(const std::size_t events, const double seconds) {
    std::lock_guard lock(mutex_);
    const auto commits = static_cast<double>(batches_.size());
    CanonicalValue::Object value{
        {"commits", CanonicalValue(commits)},
        {"commits_per_second", CanonicalValue(commits / seconds)},
        {"batch_events", Percentiles(batches_, "events")},
        {"commit_wall", Percentiles(batch_wall_ms_, "ms")},
        {"writer_cpu_ms_per_event",
         CanonicalValue(events == 0U ? 0.0 : writer_cpu_ms_ / static_cast<double>(events))},
        {"writer_cpu_share", CanonicalValue(writer_cpu_ms_ / (seconds * 1e3))},
        {"enqueue_to_commit", Percentiles(commit_ms_, "ms")},
        {"wal_max_bytes", CanonicalValue(wal_max_bytes_)},
    };
    batches_.clear();
    batch_wall_ms_.clear();
    commit_ms_.clear();
    writer_cpu_ms_ = 0.0;
    wal_max_bytes_ = 0.0;
    return value;
  }

  [[nodiscard]] int writer_nice() const { return writer_nice_; }
  [[nodiscard]] int writer_policy() const { return writer_policy_; }

private:
  std::shared_ptr<SqliteEventHistoryStore> store_;
  std::filesystem::path wal_;
  std::mutex mutex_;
  std::map<std::string, Clock::time_point> enqueued_;
  std::vector<double> batches_, batch_wall_ms_, commit_ms_;
  double writer_cpu_ms_{};
  double wal_max_bytes_{};
  int writer_nice_{1000};
  int writer_policy_{-1};
};

/// The real delivery path: producer -> bounded queue -> gap-paced writer ->
/// WAL store. Three loads per gap: steady 200/s (1 in 50 an error), a
/// back-to-back burst within the default queue, and an error storm.
CanonicalValue AsyncProfile() {
  CanonicalValue::Object gaps;
  std::uint64_t index = 0;
  for (const auto gap : {std::chrono::milliseconds(0), std::chrono::milliseconds(50)}) {
    Scratch scratch;
    const auto path = scratch.path / "history.sqlite3";
    auto store = std::make_shared<MeasuringStore>(
        std::make_shared<SqliteEventHistoryStore>(path, 100000U), path.string() + "-wal");
    AsyncEventHistorian historian(store, kDefaultHistorianQueueCapacity, gap);
    CanonicalValue::Object phases;
    struct Load { const char* name; std::size_t events; double rate; std::size_t error_every; };
    for (const auto& load : {Load{"steady_200_per_s", 2000U, 200.0, 50U},
                             Load{"burst_back_to_back", 1000U, 0.0, 0U},
                             Load{"error_storm_1000_per_s", 1000U, 1000.0, 1U}}) {
      std::vector<double> enqueue_us;
      std::size_t refused = 0;
      const auto start = Clock::now();
      for (std::size_t n = 0; n < load.events; ++n) {
        if (load.rate > 0.0) {
          std::this_thread::sleep_until(start + std::chrono::duration_cast<Clock::duration>(
                                                    std::chrono::duration<double>(n / load.rate)));
        }
        ++index;
        const bool error = load.error_every != 0U && n % load.error_every == 0U;
        auto event = MakeHistorianEvent(
            "manyforge.qualification", error ? "execute_failed" : "observation",
            error ? "ERROR" : "INFO", static_cast<std::int64_t>(index), "2026-09-08T00:00:00Z",
            Payload("small"));
        const auto enqueued = Clock::now();
        store->Enqueued(event.event_id, enqueued);
        const bool accepted = historian.TryEnqueue(std::move(event));
        enqueue_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - enqueued).count());
        refused += !accepted;
      }
      if (!historian.WaitUntilIdle(std::chrono::seconds(60)))
        throw std::runtime_error("async probe did not drain within 60 s");
      const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
      auto object = store->Take(load.events, seconds);
      object.emplace_back("events", CanonicalValue(static_cast<double>(load.events)));
      object.emplace_back("seconds", CanonicalValue(seconds));
      object.emplace_back("producer_try_enqueue", Percentiles(enqueue_us, "us"));
      object.emplace_back("refused", CanonicalValue(static_cast<double>(refused)));
      phases.emplace_back(load.name, CanonicalValue(std::move(object)));
    }
    const auto health = historian.Health();
    phases.emplace_back("historian_available", CanonicalValue(health.available));
    phases.emplace_back("writer_nice", CanonicalValue(static_cast<double>(store->writer_nice())));
    phases.emplace_back("writer_policy_is_other",
                        CanonicalValue(store->writer_policy() == SCHED_OTHER));
    gaps.emplace_back("gap_" + std::to_string(gap.count()) + "ms", CanonicalValue(std::move(phases)));
  }
  return CanonicalValue(std::move(gaps));
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string profile = argc == 2 ? argv[1] : "";
    if (profile == "async") {
      std::cout << canonical_json(CanonicalValue(CanonicalValue::Object{
                       {"schema_version", CanonicalValue("manyforge.historian_writer_probe.v1")},
                       {"profile", CanonicalValue(profile)},
                       {"gaps", AsyncProfile()},
                       {"scope", CanonicalValue("real WAL store behind the async writer; one "
                                                "producer thread; small payloads")}}))
                << '\n';
      return 0;
    }
    if (profile != "text" && profile != "tree" && profile != "rows" && profile != "capture")
      throw std::runtime_error("usage: historian_performance_probe text|tree|rows|capture|async");
    Scratch scratch;
    // The capture soak uses a 1000-row partition limit so that ordinary
    // pruning runs in both partitions alongside protected captures.
    SqliteEventHistoryStore store(scratch.path / "history.sqlite3",
                                  profile == "rows" ? 1000000 : profile == "capture" ? 1000 : 100000);
    // The capture soak uses the small payload: large text makes FTS
    // trigram indexing, not capture protection, dominate the measurement.
    const auto payload = Payload(profile);
    const std::size_t maximum_records = profile == "rows" ? 200000 : profile == "capture" ? 30000 : 148;
    std::size_t records = 0;
    bool full_retention = false;
    Measurements append, query, detail, export_page, capture_open;
    std::int64_t last_retired = 0;
    Measurements capture_retire;
    const auto start = Clock::now();
    for (std::size_t index = 1; index <= maximum_records; ++index) {
      // Capture soak: 200 routine rows with every 16th an important Stop
      // fact, then a burst of 10 errors; each burst's first error opens a
      // capture after the previous 200-row tail has closed.
      const auto cycle = (index - 1) % 210;
      const bool capture_error = profile == "capture" && cycle >= 200;
      const std::string severity = profile == "capture"
          ? (capture_error ? "ERROR" : "INFO")
          : (index % 2 == 0 ? "INFO" : "ERROR");
      const std::string event_type = profile == "capture" && !capture_error && index % 16 == 0
          ? "program_stop_completed" : "observation";
      const auto event = MakeHistorianEvent(
          "manyforge.qualification", event_type, severity,
          static_cast<std::int64_t>(index), "2026-09-08T00:00:00Z", payload);
      auto& samples = capture_error && cycle == 200 ? capture_open : append;
      if (samples.measure([&] { return store.Append(event); }) !=
          IEventHistoryStore::AppendStatus::kStored)
        throw std::runtime_error("unique event was not stored");
      records = index;
      if (profile == "capture" && capture_error && cycle == 200) {
        const auto retention = *store.Captures(std::nullopt, 1).find_object_member("retention");
        const auto retired = Count(*retention.find_object_member("captures"), "retired_count");
        if (retired != last_retired) {
          // The opening append that retired the oldest capture(s).
          capture_retire.ms.push_back(capture_open.ms.back());
          last_retired = retired;
        }
        RequireCaptureInvariants(retention, records);
      }
      if (Clock::now() - start > std::chrono::seconds(120))
        throw std::runtime_error("short probe exceeded its 120-second wall bound after " +
                                 std::to_string(records) + " stored records");
      if (profile == "rows" && records % 512 == 0) {
        HistorianQuery coverage;
        coverage.limit = 1;
        full_retention = BothPartitionsPruned(store.QuerySummaries(coverage));
        // The byte cap is reached before the configured million-row ceiling.
        // Measure the reachable full store, not an arbitrary fixed seed count.
        if (full_retention) break;
      }
    }
    if (profile == "rows" && !full_retention)
      throw std::runtime_error("probe did not reach both retention limits");
    CanonicalValue capture_result(nullptr);
    if (profile == "capture") {
      const auto capture_page = store.Captures(std::nullopt, 32);
      const auto& retention = *capture_page.find_object_member("retention");
      RequireCaptureInvariants(retention, records);
      if (last_retired < 5 || !BothPartitionsPruned(CanonicalValue(CanonicalValue::Object{
              {"retention", retention}})))
        throw std::runtime_error("capture soak did not reach capture retirement and both pruning limits: " +
                                 canonical_json(retention));
      HistorianQuery latest;
      latest.limit = 32;
      latest.capture_id = static_cast<std::uint64_t>(Count(
          capture_page.find_object_member("captures")->as_array().front(), "capture_id"));
      if (store.QuerySummaries(latest).find_object_member("events")->as_array().size() != 32U)
        throw std::runtime_error("latest capture page is incomplete");
      capture_result = CanonicalValue(CanonicalValue::Object{
          {"capture_open_append", capture_open.value()},
          {"capture_retire_append", capture_retire.ms.empty()
              ? CanonicalValue(nullptr) : capture_retire.value()},
          {"retention_captures", *retention.find_object_member("captures")}});
    }
    HistorianQuery request;
    request.limit = 32;
    std::size_t summary_bytes = 0;
    for (std::size_t index = 0; index < 20; ++index) {
      const auto page = query.measure([&] {
        const auto result = store.QuerySummaries(request);
        summary_bytes = canonical_json(result).size();
        return result;
      });
      if (page.find_object_member("events")->as_array().size() != request.limit)
        throw std::runtime_error("incomplete page");
      const auto result = detail.measure([&] { return store.Read(records); });
      if (!result) throw std::runtime_error("retained detail missing");
      (void)export_page.measure([&] {
        return canonical_json(HistorianExportPageValue(store.QueryExport(request))).size();
      });
      if (Clock::now() - start > std::chrono::seconds(120))
        throw std::runtime_error("short probe exceeded its 120-second wall bound");
    }
    const auto page = store.QuerySummaries(request);
    rusage resources{};
    if (getrusage(RUSAGE_SELF, &resources) != 0) throw std::runtime_error("getrusage failed");
    std::cout
        << canonical_json(CanonicalValue(CanonicalValue::Object{
               {"schema_version", CanonicalValue("manyforge.historian_cost_probe.v1")},
               {"profile", CanonicalValue(profile)},
               {"written_records", CanonicalValue(static_cast<double>(records))},
               {"payload_bytes",
                CanonicalValue(static_cast<double>(canonical_json(payload).size()))},
               {"summary_bytes", CanonicalValue(static_cast<double>(summary_bytes))},
               {"page_rows", CanonicalValue(32.0)},
               {"append", append.value()},
               {"summary_query", query.value()},
               {"detail", detail.value()},
               {"export_page", export_page.value()},
               {"retention", *page.find_object_member("retention")},
               {"capture", capture_result},
               {"max_rss_kib", CanonicalValue(static_cast<double>(resources.ru_maxrss))},
               {"cpu_seconds",
                CanonicalValue(resources.ru_utime.tv_sec + resources.ru_stime.tv_sec +
                               (resources.ru_utime.tv_usec + resources.ru_stime.tv_usec) / 1e6)},
               {"elapsed_seconds",
                CanonicalValue(std::chrono::duration<double>(Clock::now() - start).count())},
               {"scope", CanonicalValue("real store, serial writer/read; not concurrent "
                                        "ASGI or browser qualification")}}))
        << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
