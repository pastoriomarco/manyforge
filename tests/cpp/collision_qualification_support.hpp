#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::qualification {

inline constexpr int kUnavailableExitCode = 78;

struct AcceptanceDisposition {
  std::string_view historical_case;
  std::string_view restoration_gate;
  std::string_view required_behavior;
};

struct Distribution {
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double maximum = 0.0;
};

inline double percentile(std::vector<std::uint64_t> values,
                         const double fraction) {
  if (values.empty()) {
    return 0.0;
  }
  if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
    throw std::invalid_argument("percentile fraction must be in [0, 1]");
  }
  std::sort(values.begin(), values.end());
  const double rank =
      fraction * static_cast<double>(values.size() - static_cast<std::size_t>(1));
  const auto lower = static_cast<std::size_t>(std::floor(rank));
  const auto upper = static_cast<std::size_t>(std::ceil(rank));
  const double weight = rank - static_cast<double>(lower);
  return static_cast<double>(values[lower]) * (1.0 - weight) +
         static_cast<double>(values[upper]) * weight;
}

inline Distribution summarize_distribution(
    const std::span<const std::uint64_t> values) {
  const std::vector<std::uint64_t> copy(values.begin(), values.end());
  return Distribution{
      percentile(copy, 0.50),
      percentile(copy, 0.95),
      percentile(copy, 0.99),
      percentile(copy, 1.00),
  };
}

inline double median_absolute_deviation(
    const std::span<const std::uint64_t> values) {
  if (values.empty()) {
    return 0.0;
  }
  const std::vector<std::uint64_t> copy(values.begin(), values.end());
  const double median = percentile(copy, 0.50);
  std::vector<std::uint64_t> deviations;
  deviations.reserve(values.size());
  for (const auto value : values) {
    deviations.push_back(static_cast<std::uint64_t>(
        std::abs(static_cast<double>(value) - median)));
  }
  return percentile(std::move(deviations), 0.50);
}

// Retained, owner-neutral configuration from the historical runtime-schedule
// harness. These values are acceptance inputs, not a currently qualified
// deployment profile.
struct RuntimeScheduleContract {
  static constexpr std::int64_t kMonitorPeriodNs = 50'000'000;
  static constexpr std::int64_t kImmediateQueryBudgetNs = 50'000'000;
  static constexpr std::int64_t kLookaheadCycleBudgetNs = 200'000'000;
  static constexpr std::int64_t kLookaheadHorizonNs = 2'000'000'000;
  static constexpr std::int64_t kLookaheadQueryBudgetNs = 75'000'000;
  static constexpr std::size_t kLookaheadSamples = 32;
  static constexpr int kLookaheadMinParallelWaypoints = 8;
  static constexpr std::size_t kTrajectoryWaypointCount = 201;
  static constexpr double kTrajectoryStepS = 0.02;
};

struct ContactGraspFixtureContract {
  static constexpr double kEnvelopeRadiusM = 0.15;
  static constexpr double kEnvelopeHeightM = 0.30;
  static constexpr double kMaximumPenetrationM = 0.50;
};

struct SupportContactFixtureContract {
  static constexpr double kPositivePenetrationCapM = 0.03;
  static constexpr double kReleaseToleranceM = 0.001;
  static constexpr int kDistractorCount = 10;
};

inline int emit_unavailable(
    const std::string_view harness,
    const std::string_view restoration_gates,
    const std::span<const AcceptanceDisposition> dispositions) {
  std::cerr << "composable_cell_collision_qualification_unavailable: harness="
            << harness << " gates=" << restoration_gates
            << " mapped_acceptance_groups=" << dispositions.size()
            << " historical_source="
               "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
            << '\n';
  return kUnavailableExitCode;
}

}  // namespace manyforge::qualification
