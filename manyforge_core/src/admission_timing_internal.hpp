#pragma once

#include <manyforge_core/admission_timing_diagnostic.hpp>
#include <chrono>
#include <string_view>

namespace manyforge::core::detail {

// Direct Core calls without a host collector remain silent.
struct AdmissionTiming {
  using Clock = std::chrono::steady_clock;
  AdmissionTimingData unused{};
  AdmissionTimingData* record;
  AdmissionTiming(bool, std::string_view) noexcept
      : record(CurrentAdmissionTiming()) {
    if (!record) record = &unused;
  }
};

}  // namespace manyforge::core::detail
