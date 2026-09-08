// Short, opt-in real-storage measurements. No runtime owner or robot process.
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_headless/event_historian.hpp>
#include <stdexcept>
#include <string>
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

bool BothPartitionsPruned(const CanonicalValue& page) {
  const auto& partitions =
      page.find_object_member("retention")->find_object_member("partitions")->as_array();
  return partitions.size() == 2 &&
         std::all_of(partitions.begin(), partitions.end(), [](const auto& partition) {
           return manyforge::content_identity::parse_nonnegative_int64(
                      *partition.find_object_member("pruned_count")) >= 32;
         });
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string profile = argc == 2 ? argv[1] : "";
    if (profile != "text" && profile != "tree" && profile != "rows")
      throw std::runtime_error("usage: historian_performance_probe text|tree|rows");
    Scratch scratch;
    SqliteEventHistoryStore store(scratch.path / "history.sqlite3",
                                  profile == "rows" ? 1000000 : 100000);
    const auto payload = Payload(profile);
    const std::size_t maximum_records = profile == "rows" ? 200000 : 148;
    std::size_t records = 0;
    bool full_retention = false;
    Measurements append, query, detail, export_page;
    const auto start = Clock::now();
    for (std::size_t index = 1; index <= maximum_records; ++index) {
      const auto event = MakeHistorianEvent(
          "manyforge.qualification", "observation", index % 2 == 0 ? "INFO" : "ERROR",
          static_cast<std::int64_t>(index), "2026-09-08T00:00:00Z", payload);
      if (append.measure([&] { return store.Append(event); }) !=
          IEventHistoryStore::AppendStatus::kStored)
        throw std::runtime_error("unique event was not stored");
      records = index;
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
