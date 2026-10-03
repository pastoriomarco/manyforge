#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include <manyforge_content_identity/content_identity.hpp>

#include "manyforge_headless/operator_alarm_catalog.hpp"
#include "manyforge_headless/operator_alarm_owner.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::uint64_t kMinimumIterations = 100U;
constexpr std::uint64_t kMaximumIterations = 100000U;
constexpr std::uint64_t kMinimumOperationsPerSecond = 1000U;
constexpr std::uint64_t kMaximumP99LatencyNs = 10U * 1000U * 1000U;
constexpr std::uint64_t kMaximumSingleLatencyNs = 250U * 1000U * 1000U;
constexpr std::uint64_t kMaximumResidentSetKiB = 256U * 1024U;

struct Config final {
  std::filesystem::path catalog;
  std::uint64_t iterations{20000U};
};

struct CaseResult final {
  std::uint64_t operations{};
  std::uint64_t elapsed_ns{};
  std::uint64_t operations_per_second{};
  std::size_t live_cardinality{};
  std::uint64_t occurrence_count{};
  OperatorAlarmMetrics metrics;
};

[[noreturn]] void Fail(const std::string &message) {
  throw std::runtime_error("operator alarm performance probe: " + message);
}

std::uint64_t ParseInteger(const std::string_view text) {
  if (text.empty()) {
    Fail("iteration count is empty");
  }
  std::uint64_t value = 0U;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      Fail("iteration count must be decimal");
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      Fail("iteration count overflows");
    }
    value = value * 10U + digit;
  }
  if (value < kMinimumIterations || value > kMaximumIterations) {
    Fail("iteration count must be between 100 and 100000");
  }
  return value;
}

Config ParseArguments(const int argc, char **argv) {
  Config config;
  bool catalog_seen = false;
  bool iterations_seen = false;
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc) {
      Fail("every option requires one value");
    }
    const std::string_view option(argv[index]);
    if (option == "--catalog" && !catalog_seen) {
      config.catalog = argv[index + 1];
      catalog_seen = true;
    } else if (option == "--iterations" && !iterations_seen) {
      config.iterations = ParseInteger(argv[index + 1]);
      iterations_seen = true;
    } else {
      Fail("unknown or duplicate option " + std::string(option));
    }
  }
  if (!catalog_seen || config.catalog.empty()) {
    Fail("--catalog is required");
  }
  return config;
}

std::int64_t MonotonicNowNs() {
  const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  if (value < 0 || static_cast<std::uint64_t>(value) > kMaximumSafeInteger) {
    Fail("steady clock is outside the I-JSON range");
  }
  return value;
}

AlarmConditionUpdate Update(const std::uint64_t sequence) {
  return {
      "manyforge.runtime.provider_unavailable",
      AlarmSubject{"cell", "cell.performance", {}},
      true,
      "performance.source",
      "performance.epoch.1",
      sequence,
      static_cast<std::int64_t>(sequence),
      "2026-08-05T00:00:00Z",
      AlarmConditionQuality::kGood,
      CanonicalValue(CanonicalValue::Object{}),
      CanonicalValue(CanonicalValue::Object{}),
      {},
      CanonicalValue(CanonicalValue::Object{}),
  };
}

std::unique_ptr<AlarmLifecycleOwner>
MakeOwner(const OperatorAlarmCatalog &catalog, const std::string &owner_epoch) {
  auto owner = std::make_unique<AlarmLifecycleOwner>(
      catalog, "cell.performance", "alarm.performance",
      owner_epoch, manyforge::core::HostBootStamp{"boot.performance", 1U},
      [] { return MonotonicNowNs(); },
      [] { return std::string("2026-08-05T00:00:00Z"); },
      std::set<std::string>{"runtime_health"});
  owner->RegisterSource("runtime_health", "performance.source",
                        "manyforge.alarm_adapter.runtime_health_condition",
                        "performance.epoch.1", 1U);
  owner->CompleteSourceSnapshot("performance.source", "performance.epoch.1",
                                AlarmConditionQuality::kGood);
  return owner;
}

CaseResult MeasureRepeats(const OperatorAlarmCatalog &catalog,
                          const std::uint64_t iterations) {
  auto owner = MakeOwner(catalog, "alarm.performance.repeat");
  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t sequence = 1U; sequence <= iterations; ++sequence) {
    static_cast<void>(owner->Submit(Update(sequence)));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  const auto snapshot = owner->Snapshot();
  if (snapshot.alarms.size() != 1U) {
    Fail("repeat case did not retain exactly one live record");
  }
  const auto elapsed_ns =
      static_cast<std::uint64_t>(std::max<std::int64_t>(1, elapsed));
  return {
      iterations,
      elapsed_ns,
      iterations * 1000000000ULL / elapsed_ns,
      snapshot.live_count,
      snapshot.alarms.front().occurrence_count,
      owner->Metrics(),
  };
}

CaseResult MeasureDuplicates(const OperatorAlarmCatalog &catalog,
                             const std::uint64_t iterations) {
  auto owner = MakeOwner(catalog, "alarm.performance.duplicate");
  const auto update = Update(1U);
  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0U; index < iterations; ++index) {
    static_cast<void>(owner->Submit(update));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  const auto snapshot = owner->Snapshot();
  if (snapshot.alarms.size() != 1U) {
    Fail("duplicate case did not retain exactly one live record");
  }
  const auto elapsed_ns =
      static_cast<std::uint64_t>(std::max<std::int64_t>(1, elapsed));
  return {
      iterations,
      elapsed_ns,
      iterations * 1000000000ULL / elapsed_ns,
      snapshot.live_count,
      snapshot.alarms.front().occurrence_count,
      owner->Metrics(),
  };
}

CanonicalValue Number(const std::uint64_t value) {
  if (value > kMaximumSafeInteger) {
    Fail("result exceeds the I-JSON safe integer range");
  }
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue OptionalNumber(const std::optional<std::uint64_t> value) {
  return value.has_value() ? Number(*value) : CanonicalValue(nullptr);
}

CanonicalValue CaseValue(const CaseResult &result) {
  return CanonicalValue(CanonicalValue::Object{
      {"elapsed_ns", Number(result.elapsed_ns)},
      {"ingest_latency_sample_count",
       Number(
           static_cast<std::uint64_t>(result.metrics.ingest_latency_ns.count))},
      {"ingest_max_ns",
       OptionalNumber(result.metrics.ingest_latency_ns.maximum_ns)},
      {"ingest_p99_ns",
       OptionalNumber(result.metrics.ingest_latency_ns.p99_ns)},
      {"live_cardinality",
       Number(static_cast<std::uint64_t>(result.live_cardinality))},
      {"occurrence_count", Number(result.occurrence_count)},
      {"operations", Number(result.operations)},
      {"operations_per_second", Number(result.operations_per_second)},
  });
}

bool LatencyWithinBound(const CaseResult &result) {
  return result.metrics.ingest_latency_ns.p99_ns.value_or(
             kMaximumSafeInteger) <= kMaximumP99LatencyNs &&
         result.metrics.ingest_latency_ns.maximum_ns.value_or(
             kMaximumSafeInteger) <= kMaximumSingleLatencyNs;
}

CanonicalValue Run(const Config &config) {
  const auto catalog = OperatorAlarmCatalog::Load(config.catalog);
  const auto repeats = MeasureRepeats(catalog, config.iterations);
  const auto duplicates = MeasureDuplicates(catalog, config.iterations);
  const bool repeat_semantics = repeats.live_cardinality == 1U &&
                                repeats.occurrence_count == config.iterations;
  const bool duplicate_semantics =
      duplicates.live_cardinality == 1U && duplicates.occurrence_count == 1U;
  const bool samples_bounded =
      repeats.metrics.ingest_latency_ns.count <= 4096U &&
      duplicates.metrics.ingest_latency_ns.count <= 4096U;
  const bool throughput_bounded =
      repeats.operations_per_second >= kMinimumOperationsPerSecond &&
      duplicates.operations_per_second >= kMinimumOperationsPerSecond;
  const bool latency_bounded =
      LatencyWithinBound(repeats) && LatencyWithinBound(duplicates);
  struct rusage usage {};
  if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    Fail("getrusage failed");
  }
  const auto maximum_rss_kib = static_cast<std::uint64_t>(usage.ru_maxrss);
  const bool memory_bounded = maximum_rss_kib <= kMaximumResidentSetKiB;
  const bool passed = repeat_semantics && duplicate_semantics &&
                      samples_bounded && throughput_bounded &&
                      latency_bounded && memory_bounded;
  return CanonicalValue(CanonicalValue::Object{
      {"assertions",
       CanonicalValue(CanonicalValue::Object{
           {"duplicate_does_not_invent_occurrence",
            CanonicalValue(duplicate_semantics)},
           {"ingest_latency_within_bound", CanonicalValue(latency_bounded)},
           {"metric_samples_bounded", CanonicalValue(samples_bounded)},
           {"resident_memory_within_bound", CanonicalValue(memory_bounded)},
           {"repeat_occurrence_complete", CanonicalValue(repeat_semantics)},
           {"throughput_within_bound", CanonicalValue(throughput_bounded)},
       })},
      {"catalog_descriptor_digest",
       CanonicalValue(catalog.descriptor_digest())},
      {"duplicate_case", CaseValue(duplicates)},
      {"limits",
       CanonicalValue(CanonicalValue::Object{
           {"maximum_p99_latency_ns", Number(kMaximumP99LatencyNs)},
           {"maximum_resident_set_kib", Number(kMaximumResidentSetKiB)},
           {"maximum_single_latency_ns", Number(kMaximumSingleLatencyNs)},
           {"minimum_operations_per_second",
            Number(kMinimumOperationsPerSecond)},
       })},
      {"maximum_rss_kib", Number(maximum_rss_kib)},
      {"passed", CanonicalValue(passed)},
      {"repeat_case", CaseValue(repeats)},
      {"schema_version",
       CanonicalValue(std::string("manyforge.operator_alarm_performance.v2"))},
  });
}

} // namespace
} // namespace manyforge::headless

int main(int argc, char **argv) {
  try {
    const auto result = manyforge::headless::Run(
        manyforge::headless::ParseArguments(argc, argv));
    std::cout << manyforge::content_identity::canonical_json(result) << '\n';
    const auto *passed = result.find_object_member("passed");
    return passed != nullptr && std::get<bool>(passed->storage()) ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "operator alarm performance probe failed unexpectedly\n";
    return 2;
  }
}
