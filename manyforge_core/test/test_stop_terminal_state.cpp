#include <gtest/gtest.h>

#include <manyforge_core/stop_terminal_state.hpp>

namespace manyforge::core {
namespace {

inline constexpr char kDigestA[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
inline constexpr char kDigestB[] =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

[[nodiscard]] StopAcceptedDTO Accepted() {
  return StopAcceptedDTO{RequestId("request.1"),
                         RequestReplayStatusDTO::kAcceptedNew,
                         StopId("stop.1"),
                         StopGeneration(1U),
                         ContentDigest(kDigestA),
                         StopResolutionDispositionDTO::kExact,
                         StopScopeResolutionReasonDTO::kExplicitFullStop,
                         {},
                         ContentDigest(kDigestB),
                         {},
                         {},
                         10};
}

[[nodiscard]] StopCertificateDTO Certificate() {
  auto reduced = ReduceStopTerminal(StopTerminalReductionInputDTO{
      StopCertificateId("certificate.1"),
      StopCertificateRevision(1U),
      StopId("stop.1"),
      StopGeneration(1U),
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       ContentDigest(kDigestA), std::nullopt, std::nullopt},
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExplicitFullStop,
      {},
      {StopScopeCaptureHistoryDTO{1U,
                                  StopResolutionDispositionDTO::kExact,
                                  StopScopeResolutionReasonDTO::kExplicitFullStop,
                                  ContentDigest(kDigestB),
                                  {},
                                  {},
                                  {}}},
      {},
      {},
      {},
      std::nullopt,
      20,
      std::nullopt});
  EXPECT_TRUE(reduced.ok());
  return std::move(reduced.value());
}

[[nodiscard]] StopStatusDTO Status() {
  const auto certificate = Certificate();
  return StopStatusDTO{
      Accepted(),
      StopStatusStateDTO::kSucceeded,
      certificate.scope_entries,
      {},
      certificate.attempted_stage_results,
      certificate.evidence_items,
      certificate.unsettled,
      certificate.failed,
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                     std::nullopt},
      StopCertificateRefDTO{certificate.stop_certificate_id, certificate.stop_certificate_revision,
                            certificate.stop_id, certificate.stop_generation}};
}

TEST(StopTerminalStateTest, ValidatesExactTerminalStatusCertificateJoin) {
  const auto certificate = Certificate();
  const auto status = Status();
  EXPECT_FALSE(stop_status_field_error(status).has_value());
  EXPECT_FALSE(stop_terminal_projection_field_error(status, certificate).has_value());
}

TEST(StopTerminalStateTest, RejectsConflictingTerminalOutcomeAndProjectionPresence) {
  const auto certificate = Certificate();
  auto status = Status();
  status.state = StopStatusStateDTO::kPartial;
  EXPECT_EQ(stop_terminal_projection_field_error(status, certificate),
            "stop_terminal_projection_outcome_invalid");

  status = Status();
  status.terminal_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kFailed,
      ErrorDTO{"operation_journal_storage_failure", "failed", {}}, std::nullopt};
  EXPECT_EQ(stop_status_field_error(status), "stop_status_shape_invalid");
}

TEST(StopTerminalStateTest, TerminalStatusRestRebuildsTheExactStatus) {
  // Lean Stop record: a terminal row stores only the status rest; the rest of
  // the status is rebuilt from the certificate. The reducer fixtures
  // (test_stop_terminal_reducer) cover partial, failed, escalated and
  // predecessor-merge statuses.
  const auto certificate = Certificate();
  const auto status = Status();
  ASSERT_FALSE(stop_terminal_projection_field_error(status, certificate).has_value());
  const auto rest = TerminalStopStatusRest(status);
  EXPECT_EQ(rest.accepted, status.accepted);
  EXPECT_EQ(rest.current_stages, status.current_stages);
  EXPECT_EQ(rest.acceptance_projection, status.acceptance_projection);
  EXPECT_EQ(rest.terminal_projection.state, StopDurableProjectionStateDTO::kPending);
  EXPECT_EQ(TerminalStopStatusFromCertificate(rest, certificate), status);

  auto recorded = status;
  recorded.terminal_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kRecorded, std::nullopt, std::nullopt};
  EXPECT_EQ(TerminalStopStatusFromCertificate(TerminalStopStatusRest(recorded), certificate),
            recorded);
}

TEST(StopTerminalStateTest, ForensicProjectionResultHasClosedPresence) {
  EXPECT_FALSE(stop_forensic_projection_result_field_error(
                   StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kRecorded, 7U,
                                                   std::nullopt})
                   .has_value());
  EXPECT_FALSE(stop_forensic_projection_result_field_error(
                   StopForensicProjectionResultDTO{
                       StopForensicProjectionStateDTO::kFailed, std::nullopt,
                       ErrorDTO{"operation_journal_storage_failure", "failed", {}}})
                   .has_value());
  EXPECT_TRUE(stop_forensic_projection_result_field_error(
                  StopForensicProjectionResultDTO{StopForensicProjectionStateDTO::kRecorded,
                                                  std::nullopt, std::nullopt})
                  .has_value());
}

}  // namespace
}  // namespace manyforge::core
