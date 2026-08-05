#include "manyforge_headless/operator_alarm_owner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::int64_t kAlarmFloodWindowNs = 60LL * 1000LL * 1000LL * 1000LL;
constexpr std::int64_t kChatterWindowNs =
    10LL * 60LL * 1000LL * 1000LL * 1000LL;
constexpr std::size_t kAssessmentThreshold = 10U;
constexpr std::size_t kMaximumAssessmentIdentities = 32U;

[[noreturn]] void Fail(const std::string &message) {
  throw OperatorAlarmError("operator_alarm_metrics_invalid", message);
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label) {
  if (value > kMaximumSafeInteger) {
    Fail(std::string(label) + " exceeds the I-JSON safe integer range");
  }
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(const std::int64_t value, const std::string_view label) {
  if (value < 0 || static_cast<std::uint64_t>(value) > kMaximumSafeInteger) {
    Fail(std::string(label) + " is not a nonnegative safe integer");
  }
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue OptionalNumber(const std::optional<std::uint64_t> value,
                              const std::string_view label) {
  return value.has_value() ? Number(*value, label) : CanonicalValue(nullptr);
}

AlarmDurationSummary Summarize(std::vector<std::uint64_t> values) {
  if (values.empty()) {
    return {0U,           0U,           std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt};
  }
  std::sort(values.begin(), values.end());
  std::uint64_t sum = 0U;
  for (const auto value : values) {
    sum = std::min(kMaximumSafeInteger,
                   sum + std::min(value, kMaximumSafeInteger - sum));
  }
  const auto percentile = [&values](const double fraction) {
    const auto rank = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size())));
    return values[std::max<std::size_t>(1U, rank) - 1U];
  };
  return {
      values.size(),    sum,
      values.front(),   values.back(),
      percentile(0.50), percentile(0.95),
      percentile(0.99),
  };
}

CanonicalValue CountMap(const std::map<std::string, std::size_t> &values) {
  CanonicalValue::Object result;
  result.reserve(values.size());
  for (const auto &[key, value] : values) {
    result.emplace_back(
        key, Number(static_cast<std::uint64_t>(value), "metric count"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue TotalMap(const std::map<std::string, std::uint64_t> &values) {
  CanonicalValue::Object result;
  result.reserve(values.size());
  for (const auto &[key, value] : values) {
    result.emplace_back(key, Number(value, "metric total"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue DurationValue(const AlarmDurationSummary &summary) {
  return CanonicalValue(CanonicalValue::Object{
      {"count", Number(static_cast<std::uint64_t>(summary.count), "count")},
      {"max_ns", OptionalNumber(summary.maximum_ns, "max_ns")},
      {"min_ns", OptionalNumber(summary.minimum_ns, "min_ns")},
      {"p50_ns", OptionalNumber(summary.p50_ns, "p50_ns")},
      {"p95_ns", OptionalNumber(summary.p95_ns, "p95_ns")},
      {"p99_ns", OptionalNumber(summary.p99_ns, "p99_ns")},
      {"sum_ns", Number(summary.sum_ns, "sum_ns")},
  });
}

} // namespace

OperatorAlarmMetrics AlarmLifecycleOwner::Metrics() const {
  const auto timestamp_ns = MonotonicNow();
  std::lock_guard lock(mutex_);
  auto totals = metrics_;
  for (const auto name : {
           "accepted_updates",
           "acknowledgement_conflicts",
           "acknowledgements",
           "activations",
           "bulk_acknowledgements",
           "capacity_rejections",
           "conflicting_updates",
           "duplicate_updates",
           "historian_delivery_failures",
           "historian_delivery_recoveries",
           "out_of_order_sequence_updates",
           "owner_restarts",
           "recurrences",
           "rejected_updates",
           "repeat_occurrences",
           "returns",
           "source_quality_unknown_observations",
           "stale_epoch_updates",
           "stale_updates",
       }) {
    totals.try_emplace(name, 0U);
  }

  std::map<std::string, std::size_t> counts_by_state{
      {"active_acknowledged", 0U},
      {"active_unacknowledged", 0U},
      {"returned_acknowledged", 0U},
      {"returned_unacknowledged", 0U},
  };
  std::map<std::string, std::size_t> counts_by_priority{
      {"critical", 0U}, {"high", 0U}, {"low", 0U}, {"medium", 0U}};
  std::map<std::string, std::size_t> counts_by_class;
  std::size_t live_alarms = 0U;
  std::size_t attention = 0U;
  std::size_t motion_attention = 0U;
  std::size_t active_standing = 0U;
  std::vector<std::uint64_t> current_standing;
  for (const auto &[condition_id, point] : points_) {
    static_cast<void>(condition_id);
    if (!point.record.has_value() ||
        point.record->lifecycle_state ==
            AlarmLifecycleState::kReturnedAcknowledged) {
      continue;
    }
    const auto &record = *point.record;
    ++live_alarms;
    ++counts_by_state[std::string(ToString(record.lifecycle_state))];
    ++counts_by_priority[record.priority];
    ++counts_by_class[record.alarm_class];
    if (RequiresAttention(record.lifecycle_state)) {
      ++attention;
      motion_attention += record.requires_motion_attention ? 1U : 0U;
    }
    if (record.lifecycle_state == AlarmLifecycleState::kActiveAcknowledged) {
      ++active_standing;
      if (record.acknowledged_at_ns.has_value()) {
        current_standing.push_back(
            timestamp_ns >= *record.acknowledged_at_ns
                ? static_cast<std::uint64_t>(timestamp_ns -
                                             *record.acknowledged_at_ns)
                : 0U);
      }
    }
  }
  const auto unknown_sources = static_cast<std::size_t>(
      std::count_if(sources_.begin(), sources_.end(), [](const auto &entry) {
        return !entry.second.snapshot_complete ||
               entry.second.quality != AlarmConditionQuality::kGood;
      }));
  const auto flood_start = timestamp_ns >= kAlarmFloodWindowNs
                               ? timestamp_ns - kAlarmFloodWindowNs
                               : 0;
  const auto flood_count = static_cast<std::size_t>(std::count_if(
      recent_activations_ns_.begin(), recent_activations_ns_.end(),
      [flood_start](const auto value) { return value >= flood_start; }));
  const auto chatter_start =
      timestamp_ns >= kChatterWindowNs ? timestamp_ns - kChatterWindowNs : 0;
  std::vector<AlarmChatteringCandidate> chatter;
  for (const auto &[condition_id, activations] : condition_activations_ns_) {
    const auto count = static_cast<std::size_t>(std::count_if(
        activations.begin(), activations.end(),
        [chatter_start](const auto value) { return value >= chatter_start; }));
    if (count > kAssessmentThreshold) {
      chatter.push_back({condition_id, count});
    }
  }
  std::sort(chatter.begin(), chatter.end(),
            [](const auto &left, const auto &right) {
              if (left.activations_in_window != right.activations_in_window) {
                return left.activations_in_window > right.activations_in_window;
              }
              return left.condition_id < right.condition_id;
            });
  const bool chatter_truncated = chatter.size() > kMaximumAssessmentIdentities;
  if (chatter_truncated) {
    chatter.resize(kMaximumAssessmentIdentities);
  }
  return {
      owner_id_,
      owner_epoch_,
      generation_,
      state_revision_,
      std::move(totals),
      points_.size(),
      live_alarms,
      attention,
      motion_attention,
      active_standing,
      terminal_records_.size(),
      unknown_sources,
      std::move(counts_by_state),
      std::move(counts_by_priority),
      std::move(counts_by_class),
      Summarize(std::vector<std::uint64_t>(ingest_latency_ns_.begin(),
                                           ingest_latency_ns_.end())),
      Summarize(std::vector<std::uint64_t>(acknowledgement_latency_ns_.begin(),
                                           acknowledgement_latency_ns_.end())),
      Summarize(
          std::vector<std::uint64_t>(completed_standing_duration_ns_.begin(),
                                     completed_standing_duration_ns_.end())),
      Summarize(std::move(current_standing)),
      flood_count > kAssessmentThreshold,
      flood_count,
      std::move(chatter),
      chatter_truncated,
      timestamp_ns,
  };
}

CanonicalValue OperatorAlarmMetricsValue(const OperatorAlarmMetrics &metrics) {
  CanonicalValue::Array chatter;
  chatter.reserve(metrics.chattering_candidates.size());
  for (const auto &candidate : metrics.chattering_candidates) {
    chatter.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"activations_in_window",
         Number(static_cast<std::uint64_t>(candidate.activations_in_window),
                "activations_in_window")},
        {"condition_id", CanonicalValue(candidate.condition_id)},
    }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"assessment",
       CanonicalValue(CanonicalValue::Object{
           {"activations_in_flood_window",
            Number(
                static_cast<std::uint64_t>(metrics.activations_in_flood_window),
                "activations_in_flood_window")},
           {"alarm_flood_candidate",
            CanonicalValue(metrics.alarm_flood_candidate)},
           {"alarm_flood_threshold",
            Number(kAssessmentThreshold, "alarm_flood_threshold")},
           {"alarm_flood_window_ns",
            Number(kAlarmFloodWindowNs, "alarm_flood_window_ns")},
           {"chatter_threshold",
            Number(kAssessmentThreshold, "chatter_threshold")},
           {"chatter_window_ns", Number(kChatterWindowNs, "chatter_window_ns")},
           {"chattering_candidates", CanonicalValue(std::move(chatter))},
           {"chattering_candidates_truncated",
            CanonicalValue(metrics.chattering_candidates_truncated)},
           {"claim_boundary",
            CanonicalValue("engineering_assessment_not_safety_limit")},
       })},
      {"distributions",
       CanonicalValue(CanonicalValue::Object{
           {"acknowledgement_latency_ns",
            DurationValue(metrics.acknowledgement_latency_ns)},
           {"completed_standing_duration_ns",
            DurationValue(metrics.completed_standing_duration_ns)},
           {"current_standing_duration_ns",
            DurationValue(metrics.current_standing_duration_ns)},
           {"ingest_latency_ns", DurationValue(metrics.ingest_latency_ns)},
       })},
      {"gauges",
       CanonicalValue(CanonicalValue::Object{
           {"active_standing",
            Number(static_cast<std::uint64_t>(metrics.active_standing),
                   "active_standing")},
           {"counts_by_class", CountMap(metrics.counts_by_class)},
           {"counts_by_priority", CountMap(metrics.counts_by_priority)},
           {"counts_by_state", CountMap(metrics.counts_by_state)},
           {"generation", Number(metrics.generation, "generation")},
           {"live_alarms",
            Number(static_cast<std::uint64_t>(metrics.live_alarms),
                   "live_alarms")},
           {"live_condition_points",
            Number(static_cast<std::uint64_t>(metrics.live_condition_points),
                   "live_condition_points")},
           {"motion_attention",
            Number(static_cast<std::uint64_t>(metrics.motion_attention),
                   "motion_attention")},
           {"state_revision", Number(metrics.state_revision, "state_revision")},
           {"terminal_records",
            Number(static_cast<std::uint64_t>(metrics.terminal_records),
                   "terminal_records")},
           {"unacknowledged_attention",
            Number(static_cast<std::uint64_t>(metrics.unacknowledged_attention),
                   "unacknowledged_attention")},
           {"unknown_quality_sources",
            Number(static_cast<std::uint64_t>(metrics.unknown_quality_sources),
                   "unknown_quality_sources")},
       })},
      {"owner_epoch", CanonicalValue(metrics.owner_epoch)},
      {"owner_id", CanonicalValue(metrics.owner_id)},
      {"schema_version",
       CanonicalValue(std::string(kOperatorAlarmMetricsApiVersion))},
      {"timestamp_ns", Number(metrics.timestamp_ns, "timestamp_ns")},
      {"totals", TotalMap(metrics.totals)},
  });
}

} // namespace manyforge::headless
