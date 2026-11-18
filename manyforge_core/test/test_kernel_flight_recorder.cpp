#include <manyforge_core/kernel_flight_recorder.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

KernelEvent make_event(std::uint64_t execution_id,
                       KernelEventType type,
                       std::int64_t timestamp_ns,
                       std::optional<ErrorDTO> error = std::nullopt) {
  KernelEvent event;
  event.type = type;
  event.execution_id = execution_id;
  event.timestamp_ns = timestamp_ns;
  event.from_state = ExecutionState::kIdle;
  event.to_state = ExecutionState::kPlanning;
  event.error = std::move(error);
  return event;
}

// The dump's lines, read from stderr between its BEGIN and END markers.
std::vector<std::string> dump_lines(KernelFlightRecorder& recorder,
                                    const char* reason) {
  testing::internal::CaptureStderr();
  recorder.dump_now(reason);
  std::istringstream in(testing::internal::GetCapturedStderr());
  std::vector<std::string> lines;
  std::string line;
  bool inside = false;
  while (std::getline(in, line)) {
    if (line == "MANYFORGE_KERNEL_FLIGHT_DUMP_BEGIN") {
      inside = true;
    } else if (line == "MANYFORGE_KERNEL_FLIGHT_DUMP_END") {
      inside = false;
    } else if (inside) {
      lines.push_back(line);
    }
  }
  return lines;
}

// Every emitted line must be a structurally complete JSON object: dumps are
// consumed post-mortem by the composer ingest, and a torn line would poison
// the whole file.
void expect_line_valid(const std::string& line) {
  ASSERT_FALSE(line.empty());
  EXPECT_EQ(line.front(), '{');
  EXPECT_EQ(line.back(), '}');
}

TEST(KernelFlightRecorderTest, ContactPolicyViolationTypeNameIsStable) {
  EXPECT_STREQ(kernel_event_type_name(KernelEventType::kContactPolicyViolation),
               "contact_policy_violation");
}

TEST(KernelFlightRecorderTest, ParticipantDispatchOutcomeTypeNameIsStable) {
  EXPECT_STREQ(kernel_event_type_name(KernelEventType::kParticipantDispatchOutcome),
               "participant_dispatch_outcome");
}

TEST(KernelFlightRecorderTest, RingWrapsKeepingNewestRecords) {
  KernelFlightRecorder recorder;
  const auto total = KernelFlightRecorder::kCapacity + 88;
  for (std::uint64_t i = 1; i <= total; ++i) {
    recorder.record(make_event(i, KernelEventType::kStateTransition,
                               static_cast<std::int64_t>(i)));
  }
  EXPECT_EQ(recorder.recorded_total(), total);

  const auto lines = dump_lines(recorder, "unit_test");
  // header + kCapacity records (no snapshot note in this test)
  ASSERT_EQ(lines.size(), 1 + KernelFlightRecorder::kCapacity);
  for (const auto& line : lines) {
    expect_line_valid(line);
  }
  // Oldest surviving sequence is total - kCapacity + 1; newest is total.
  const auto oldest = total - KernelFlightRecorder::kCapacity + 1;
  EXPECT_NE(lines[1].find("\"seq\":" + std::to_string(oldest)),
            std::string::npos);
  EXPECT_NE(lines.back().find("\"seq\":" + std::to_string(total)),
            std::string::npos);
}

TEST(KernelFlightRecorderTest, DumpCarriesErrorCodeAndSnapshotNote) {
  KernelFlightRecorder recorder;
  recorder.record(make_event(
      7, KernelEventType::kErrorRaised, 123,
      ErrorDTO{"planning_failed", "boom", {}}));
  recorder.note_published_snapshot(41, 456, 7, ExecutionState::kFailed);

  const auto lines = dump_lines(recorder, "unit_test");
  ASSERT_EQ(lines.size(), 3u);  // header + record + snapshot note
  EXPECT_NE(lines[0].find("\"schema_version\":\"manyforge.kernel_flight.v1\""),
            std::string::npos);
  EXPECT_NE(lines[0].find("\"reason\":\"unit_test\""), std::string::npos);
  EXPECT_NE(lines[1].find("\"type\":\"error_raised\""), std::string::npos);
  EXPECT_NE(lines[1].find("\"error_code\":\"planning_failed\""),
            std::string::npos);
  EXPECT_NE(lines[2].find("\"snapshot\":"), std::string::npos);
  EXPECT_NE(lines[2].find("\"sequence_number\":41"), std::string::npos);
}

TEST(KernelFlightRecorderTest, ErrorCodeIsTruncatedNotOverrun) {
  KernelFlightRecorder recorder;
  const std::string long_code(200, 'x');
  recorder.record(make_event(1, KernelEventType::kErrorRaised, 1,
                             ErrorDTO{long_code, "m", {}}));
  const auto lines = dump_lines(recorder, "unit_test");
  ASSERT_EQ(lines.size(), 2u);
  expect_line_valid(lines[1]);
  // Truncated to the fixed inline buffer (23 chars + NUL).
  EXPECT_NE(lines[1].find("\"error_code\":\"" + std::string(23, 'x') + "\""),
            std::string::npos);
}

TEST(KernelFlightRecorderTest, DumpUnderConcurrentEmissionStaysLineValid) {
  KernelFlightRecorder recorder;

  std::atomic<bool> stop{false};
  std::vector<std::jthread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&recorder, &stop, t]() {
      std::uint64_t i = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        recorder.record(make_event(static_cast<std::uint64_t>(t),
                                   KernelEventType::kStateTransition,
                                   static_cast<std::int64_t>(++i)));
      }
    });
  }
  std::vector<std::vector<std::string>> dumps;
  for (int d = 0; d < 20; ++d) {
    dumps.push_back(dump_lines(recorder, "concurrent_test"));
  }
  stop.store(true);
  writers.clear();

  for (const auto& lines : dumps) {
    // A busy recorder lock is an intentional crash-context fallback: dump_now
    // emits a minimal line-valid torn marker. Dumps that did acquire the lock
    // must remain valid too.
    for (const auto& line : lines) {
      expect_line_valid(line);
    }
  }
}

TEST(KernelFlightRecorderTest, OutOfOrderKernelSequencesDoNotRegressTotals) {
  KernelFlightRecorder recorder;
  // Kernel-assigned sequences arriving out of dispatch order (2 before 1):
  // the high-water mark must not regress, and the dump must contain both.
  KernelEvent second = make_event(2, KernelEventType::kStateTransition, 20);
  second.sequence = 2;
  KernelEvent first = make_event(1, KernelEventType::kStateTransition, 10);
  first.sequence = 1;
  recorder.record(second);
  recorder.record(first);
  EXPECT_EQ(recorder.recorded_total(), 2u);

  const auto lines = dump_lines(recorder, "unit_test");
  ASSERT_EQ(lines.size(), 3u);  // header + 2 records
  EXPECT_NE(lines[1].find("\"seq\":1"), std::string::npos);
  EXPECT_NE(lines[2].find("\"seq\":2"), std::string::npos);
}

}  // namespace
}  // namespace manyforge::core
