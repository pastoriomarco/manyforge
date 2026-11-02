#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace manyforge::core::detail {

enum class AdmissionJournalRowsKind { kReturnedRows, kTableCardinality };

// Active within the host's synchronous admission or selected-finalize dispatch.
struct AdmissionTimingData {
  using Duration = std::chrono::nanoseconds;
  struct JournalEvent {
    const char* stage{};  // static string literal
    std::int64_t elapsed_ns{};
    std::int64_t rows{};
    std::int64_t changes{};
    const char* outcome{};  // static string literal, or null
    AdmissionJournalRowsKind rows_kind{AdmissionJournalRowsKind::kReturnedRows};
  };
  Duration submission_wait{}, admission_wait{}, transition_wait{}, owner_wait{};
  Duration settlement{}, journal{}, refresh{};
  std::array<JournalEvent, 64> events{};
  std::size_t event_count{};
  std::size_t dropped{};
};

AdmissionTimingData* CurrentAdmissionTiming() noexcept;
void SetAdmissionTiming(AdmissionTimingData*) noexcept;
void CaptureAdmissionJournalTiming(const char* stage, std::int64_t elapsed_ns,
                                   std::int64_t rows, std::int64_t changes,
                                   const char* outcome = nullptr,
                                   AdmissionJournalRowsKind rows_kind =
                                       AdmissionJournalRowsKind::kReturnedRows) noexcept;

struct AdmissionTimingScope {
  AdmissionTimingData data{};
  AdmissionTimingData* previous{CurrentAdmissionTiming()};
  AdmissionTimingScope() noexcept { SetAdmissionTiming(&data); }
  ~AdmissionTimingScope() noexcept { SetAdmissionTiming(previous); }
};

}  // namespace manyforge::core::detail
