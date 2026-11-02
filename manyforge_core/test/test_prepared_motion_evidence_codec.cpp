#include <gtest/gtest.h>

#include <manyforge_core/prepared_motion_evidence_codec.hpp>

namespace manyforge::core {
namespace {

[[nodiscard]] std::string Digest(char value) {
  return "sha256:" + std::string(64U, value);
}

[[nodiscard]] TrajectoryDTO Trajectory() {
  return {{ResourceId("joint.1")}, {"joint_1"}, {{0.0, {0.0}, {}, {}}, {1.0, {0.5}, {0.0}, {0.0}}}};
}

[[nodiscard]] ProjectionRevisionDTO Projection() {
  return {RetainedBundleId(Digest('1')), GraphRevision(Digest('2')),
          SceneRevision{SceneId(Digest('3')), 4U, ContentDigest(Digest('4'))}};
}

[[nodiscard]] PreparedMotionPlanningEvidenceDTO Planning() {
  return {PlanResultDTO{Trajectory(), std::nullopt, Projection()}, 2U,
          PlannerCapabilityBindingDTO{
              CapabilityId("planning.motion.joint_goal"), CapabilityDefinitionRevision(Digest('5')),
              CapabilityProviderBindingId(Digest('6')),
              CapabilityProviderBindingRevision(Digest('7')), ProviderInstanceId("planner.1"),
              ProviderManifestId("planner.manifest"), ProviderManifestRevision(Digest('9')),
              RuntimeUnitId("planner.runtime"), RuntimeEpoch("planner.epoch")}};
}

[[nodiscard]] PreparedMotionValidationEvidenceDTO Validation() {
  const CollisionWorldRevision world{3U, Digest('a')};
  CollisionDecision decision;
  decision.accepted = true;
  decision.severity = CollisionDecisionSeverity::kNone;
  decision.action = CollisionDecisionAction::kDispatch;
  decision.collision_world_revision = world;
  return {ValidationRevision("validation.3"),
          ValidationResultDTO{true, std::nullopt,
                              TrajectoryValidationMetricsDTO{1.0, {0.5}, {0.0}, 0.02}},
          world, CollisionPolicyRevision{5U, Digest('b')}, std::move(decision)};
}

TEST(PreparedMotionEvidenceCodecTest, RoundTripsAcceptedPlanningAndValidationEvidence) {
  const auto planning = EncodePreparedMotionPlanningEvidence(Planning());
  ASSERT_TRUE(planning.ok()) << planning.error().message;
  const auto decoded_planning = DecodePreparedMotionPlanningEvidence(planning.value());
  ASSERT_TRUE(decoded_planning.ok()) << decoded_planning.error().message;
  EXPECT_EQ(decoded_planning.value().planning_attempt_index, 2U);
  EXPECT_EQ(decoded_planning.value().planner_capability_binding.provider_manifest_id,
            ProviderManifestId("planner.manifest"));
  EXPECT_EQ(EncodePreparedMotionPlanningEvidence(decoded_planning.value()).value(),
            planning.value());

  const auto validation = EncodePreparedMotionValidationEvidence(Validation());
  ASSERT_TRUE(validation.ok()) << validation.error().message;
  const auto decoded_validation = DecodePreparedMotionValidationEvidence(validation.value());
  ASSERT_TRUE(decoded_validation.ok()) << decoded_validation.error().message;
  EXPECT_EQ(decoded_validation.value().collision_world_revision.revision_id, 3U);
  ASSERT_TRUE(decoded_validation.value().noncollision_result.trajectory_metrics.has_value());
  EXPECT_EQ(decoded_validation.value()
                .noncollision_result.trajectory_metrics->peak_abs_joint_velocity_rad_s,
            (std::vector<double>{0.5}));
  EXPECT_EQ(decoded_validation.value()
                .noncollision_result.trajectory_metrics->peak_abs_joint_acceleration_rad_s2,
            (std::vector<double>{0.0}));
  EXPECT_DOUBLE_EQ(
      *decoded_validation.value().noncollision_result.trajectory_metrics->peak_tcp_linear_speed_mps,
      0.02);
  EXPECT_EQ(EncodePreparedMotionValidationEvidence(decoded_validation.value()).value(),
            validation.value());
}

TEST(PreparedMotionEvidenceCodecTest, RejectsMalformedManifestIdsAndKeepsRevisionDigestStrict) {
  for (const auto& invalid_id : std::vector<std::string>{
           "", "manifest/invalid", "manifest invalid", Digest('8'),
           std::string("manifest\0invalid", 16U), std::string(129U, 'a')}) {
    SCOPED_TRACE(invalid_id);
    auto invalid = Planning();
    invalid.planner_capability_binding.provider_manifest_id = ProviderManifestId(invalid_id);
    const auto encoded = EncodePreparedMotionPlanningEvidence(invalid);
    ASSERT_TRUE(encoded.ok());
    const auto decoded = DecodePreparedMotionPlanningEvidence(encoded.value());
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().details.at("reason"), "planner_capability_binding_invalid");
  }
  auto invalid_revision = Planning();
  invalid_revision.planner_capability_binding.provider_manifest_revision =
      ProviderManifestRevision("logical.revision");
  const auto encoded = EncodePreparedMotionPlanningEvidence(invalid_revision);
  ASSERT_TRUE(encoded.ok());
  EXPECT_FALSE(DecodePreparedMotionPlanningEvidence(encoded.value()).ok());
}

TEST(PreparedMotionEvidenceCodecTest,
     RoundTripsAcceptedCartesianFidelityDiagnosticsWithoutChangingLegacyShape) {
  auto cartesian = Validation();
  auto& metrics = *cartesian.noncollision_result.trajectory_metrics;
  metrics.cartesian_fidelity_metrics = CartesianFidelityMetricsDTO{
      0.005, 0.05, 0.001, 0.01, 0.0005, 0.002, 0.0015, 0.012};
  const auto encoded = EncodePreparedMotionValidationEvidence(cartesian);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  const auto decoded = DecodePreparedMotionValidationEvidence(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  ASSERT_TRUE(decoded.value()
                  .noncollision_result.trajectory_metrics->cartesian_fidelity_metrics.has_value());
  EXPECT_EQ(*decoded.value().noncollision_result.trajectory_metrics->cartesian_fidelity_metrics,
            *metrics.cartesian_fidelity_metrics);

  const auto legacy = EncodePreparedMotionValidationEvidence(Validation());
  ASSERT_TRUE(legacy.ok());
  EXPECT_EQ(legacy.value().find("cartesian_fidelity_metrics"), std::string::npos);
}

TEST(PreparedMotionEvidenceCodecTest, RejectsFailureEvidenceAndClosedShapeViolations) {
  auto invalid = Validation();
  invalid.collision_decision.failure_archive_id = "archive.1";
  EXPECT_FALSE(EncodePreparedMotionValidationEvidence(invalid).ok());

  const auto encoded = EncodePreparedMotionValidationEvidence(Validation());
  ASSERT_TRUE(encoded.ok());
  auto unknown = encoded.value();
  unknown.insert(unknown.size() - 1U, ",\"unknown\":true");
  EXPECT_FALSE(DecodePreparedMotionValidationEvidence(unknown).ok());

  auto mismatched = Validation();
  mismatched.collision_decision.collision_world_revision->revision_id += 1U;
  EXPECT_FALSE(EncodePreparedMotionValidationEvidence(mismatched).ok());

  auto missing_metrics = Validation();
  missing_metrics.noncollision_result.trajectory_metrics.reset();
  EXPECT_FALSE(EncodePreparedMotionValidationEvidence(missing_metrics).ok());
}

TEST(PreparedMotionEvidenceCodecTest, RoundTripsOneNestedMovingPairAndRejectsMalformedMetrics) {
  auto evidence = Validation();
  evidence.noncollision_result.moving_replacement = MovingReplacementValidationEvidenceDTO{
      TrajectoryValidationMetricsDTO{0.25, {0.3}, {0.0}, 0.02},
      TrajectoryValidationMetricsDTO{0.75, {0.5}, {0.0}, 0.01}};
  const auto encoded = EncodePreparedMotionValidationEvidence(evidence);
  ASSERT_TRUE(encoded.ok()) << encoded.error().details.at("reason");
  const auto decoded = DecodePreparedMotionValidationEvidence(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().details.at("reason");
  EXPECT_EQ(decoded.value().noncollision_result.moving_replacement,
            evidence.noncollision_result.moving_replacement);
  EXPECT_EQ(EncodePreparedMotionValidationEvidence(decoded.value()).value(), encoded.value());
  evidence.noncollision_result.moving_replacement->changed_future.peak_abs_joint_velocity_rad_s[0] =
      -0.1;
  EXPECT_FALSE(EncodePreparedMotionValidationEvidence(evidence).ok());
  auto unknown = encoded.value();
  const auto moving = unknown.find("\"moving_replacement\":{");
  ASSERT_NE(moving, std::string::npos);
  unknown.insert(moving + std::string("\"moving_replacement\":{").size(), "\"unknown\":true,");
  EXPECT_FALSE(DecodePreparedMotionValidationEvidence(unknown).ok());
  const CartesianFidelityMetricsDTO fidelity{0.005,  0.05,  0.001, 0.01,
                                             0.0005, 0.002, 0.0015, 0.012};
  auto bound_fidelity = Validation();
  bound_fidelity.noncollision_result.trajectory_metrics->cartesian_fidelity_metrics = fidelity;
  const auto bound_bytes = EncodePreparedMotionValidationEvidence(bound_fidelity);
  ASSERT_TRUE(bound_bytes.ok());
  const auto fidelity_begin = bound_bytes.value().find("\"cartesian_fidelity_metrics\":{");
  ASSERT_NE(fidelity_begin, std::string::npos);
  const auto fidelity_end = bound_bytes.value().find('}', fidelity_begin);
  ASSERT_NE(fidelity_end, std::string::npos);
  const auto fidelity_field =
      bound_bytes.value().substr(fidelity_begin, fidelity_end - fidelity_begin + 1) + ",";
  for (const bool prefix : {true, false}) {
    SCOPED_TRACE(prefix);
    auto invalid = decoded.value();
    auto& pair = *invalid.noncollision_result.moving_replacement;
    (prefix ? pair.unchanged_prefix : pair.changed_future).cartesian_fidelity_metrics = fidelity;
    EXPECT_FALSE(EncodePreparedMotionValidationEvidence(invalid).ok());
    auto unbound_bytes = encoded.value();
    const std::string piece = prefix ? "\"unchanged_prefix\":{" : "\"changed_future\":{";
    const auto piece_begin = unbound_bytes.find(piece);
    ASSERT_NE(piece_begin, std::string::npos);
    unbound_bytes.insert(piece_begin + piece.size(), fidelity_field);
    EXPECT_FALSE(DecodePreparedMotionValidationEvidence(unbound_bytes).ok());
  }
  const auto ordinary = EncodePreparedMotionValidationEvidence(Validation());
  ASSERT_TRUE(ordinary.ok());
  EXPECT_EQ(ordinary.value().find("moving_replacement"), std::string::npos);
}

}  // namespace
}  // namespace manyforge::core
