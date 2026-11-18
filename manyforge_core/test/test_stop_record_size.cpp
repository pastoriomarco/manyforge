// Stop state model (design 2026-11-13 §2.6): the Stop state is bounded by
// configuration (N_max, R) and the applied cell, never by how many Stops
// happened. These tests pin the fixed sizes the design's memory table states.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <manyforge_core/stop_state.hpp>

namespace manyforge::core {
namespace {

constexpr std::size_t kProductionMaxLiveStopParticipants = 64U;  // N_max
constexpr std::size_t kProductionRecords = 1024U;                // R (OP-2)

[[nodiscard]] std::size_t record_bytes(const std::size_t lanes) {
  return sizeof(StopEventRecord) + lanes * sizeof(StopCoveredEntry) +
         lanes * sizeof(StopStaleEntry) + ((lanes + 63U) / 64U) * sizeof(std::uint64_t);
}

TEST(StopRecordSizeTest, CoveredEntryStaysCompact) {
  // ≈ 32 B per covered participant (§2.4); kept below 48 B.
  EXPECT_LE(sizeof(StopCoveredEntry), 48U);
}

TEST(StopRecordSizeTest, EventRecordFixedPartStaysWithinOneKiB) {
  // Principal 256 B + reason 256 B + label 64 B + header (§2.4 ≈ 720 B).
  EXPECT_LE(sizeof(StopEventRecord), 1024U);
}

TEST(StopRecordSizeTest, ProductionRingStaysWithinSixMiB) {
  // §2.6: ≈ 4.4 MB at N_max = 64, R = 1024, plus the 2R index buckets.
  const auto ring = kProductionRecords * record_bytes(kProductionMaxLiveStopParticipants) +
                    2U * kProductionRecords * sizeof(std::uint32_t);
  EXPECT_LE(ring, 6U * 1024U * 1024U);
}

TEST(StopRecordSizeTest, ParticipantStopStateStaysSmall) {
  // §2.1: about 72 bytes plus the optional error.
  EXPECT_LE(sizeof(ParticipantStopState), 72U + sizeof(std::optional<ErrorDTO>));
}

TEST(StopRecordSizeTest, LaneFreeListEntryIsFixed) {
  // §2.6: N_max × 16 B.
  EXPECT_LE(sizeof(StopLaneFreeList::Owner) + sizeof(std::uint16_t), 24U);
}

}  // namespace
}  // namespace manyforge::core
