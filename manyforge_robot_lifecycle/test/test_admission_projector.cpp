// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include <manyforge_robot_lifecycle/admission_projector.hpp>

namespace manyforge::robot_lifecycle
{
namespace
{

constexpr std::int64_t kNow = 1'000'000'000;
constexpr std::int64_t kMaximumLease = 250'000'000;

AdmissionProjector make_projector()
{
  AdmissionProjectorConfig config;
  config.provider_instance_id = "provider-instance-1";
  config.robot_id = "lite6-a";
  config.binding_contract_generation_id = "binding-generation-1";
  config.maximum_lease_ns = kMaximumLease;
  config.runtime_generation_id = "runtime-generation-1";
  config.provider_component_epoch = "provider-epoch-1";
  config.cell_supervisor_component_epoch = "supervisor-epoch-1";
  return AdmissionProjector(config);
}

LifecycleSnapshot ready_snapshot()
{
  LifecycleSnapshot snapshot;
  snapshot.robot_id = "lite6-a";
  snapshot.supervisor_instance_id = "supervisor-instance-1";
  snapshot.authority_profile = AuthorityProfile::kLocalOperator;
  snapshot.generation = 10;
  snapshot.authority_session = AuthoritySession{
    AuthorityProfile::kLocalOperator,
    "operator-session-1",
    3,
    "human:operator",
    kNow + 500'000'000,
    "operator-epoch-1"};
  snapshot.observed.generation = 4;
  snapshot.observed.transport_owner_component_epoch = "owner-epoch-1";
  snapshot.observed.source_timestamp_ns = kNow - 1'000'000;
  snapshot.observed.received_timestamp_ns = kNow;
  snapshot.observed.fresh_until_ns = kNow + 300'000'000;
  snapshot.observation_fresh = true;
  snapshot.authority_fresh = true;
  snapshot.observe_ready = true;
  snapshot.controller_ready = true;
  snapshot.motion_admitted = true;
  return snapshot;
}

TEST(AdmissionProjectorTest, RejectsIncompleteConfiguration) {
  EXPECT_THROW(
    AdmissionProjector(
      AdmissionProjectorConfig{
        "", "lite6-a", "binding-generation-1", kMaximumLease,
        "runtime-generation-1", "provider-epoch-1",
        "supervisor-epoch-1"}),
    std::invalid_argument);
  EXPECT_THROW(
    AdmissionProjector(
      AdmissionProjectorConfig{
        "provider-instance-1", "lite6-a", "binding-generation-1", 0,
        "runtime-generation-1", "provider-epoch-1",
        "supervisor-epoch-1"}),
    std::invalid_argument);
}

TEST(AdmissionProjectorTest, PublishesBoundedReadyAdmission) {
  auto projector = make_projector();
  const auto publication = projector.project(ready_snapshot(), kNow);

  ASSERT_TRUE(publication.has_value());
  EXPECT_EQ(publication->publication_generation, 1U);
  EXPECT_EQ(publication->lifecycle_generation, 1U);
  EXPECT_EQ(publication->source_snapshot_generation, 10U);
  EXPECT_EQ(publication->lease_duration_ns, kMaximumLease);
  EXPECT_TRUE(publication->controller_ready);
  EXPECT_TRUE(publication->motion_admitted);
  EXPECT_TRUE(publication->reason_codes.empty());
  EXPECT_EQ(publication->runtime_generation_id, "runtime-generation-1");
  EXPECT_EQ(publication->provider_component_epoch, "provider-epoch-1");
  EXPECT_EQ(
    publication->transport_owner_component_epoch, "owner-epoch-1");
  EXPECT_EQ(publication->authority_component_epoch, "operator-epoch-1");
  EXPECT_EQ(
    publication->cell_supervisor_component_epoch, "supervisor-epoch-1");
}

TEST(AdmissionProjectorTest, LeaseOnlyRenewalKeepsLifecycleGeneration) {
  auto projector = make_projector();
  ASSERT_TRUE(projector.project(ready_snapshot(), kNow).has_value());

  auto renewed = ready_snapshot();
  renewed.observed.received_timestamp_ns += 50'000'000;
  renewed.observed.fresh_until_ns += 50'000'000;
  renewed.authority_session->valid_until_ns += 50'000'000;
  const auto publication =
    projector.project(renewed, kNow + 50'000'000);

  ASSERT_TRUE(publication.has_value());
  EXPECT_EQ(publication->publication_generation, 2U);
  EXPECT_EQ(publication->lifecycle_generation, 1U);
}

TEST(AdmissionProjectorTest, TimeDerivedExpiryAdvancesSemanticGeneration) {
  auto projector = make_projector();
  ASSERT_TRUE(projector.project(ready_snapshot(), kNow).has_value());

  auto expired = ready_snapshot();
  expired.observation_fresh = false;
  expired.controller_ready = false;
  expired.motion_admitted = false;
  expired.admission_reason_codes = {
    "transport_not_connected",
    "observation_stale_or_missing",
  };
  const auto publication =
    projector.project(expired, kNow + 400'000'000);

  ASSERT_TRUE(publication.has_value());
  EXPECT_EQ(publication->source_snapshot_generation, 10U);
  EXPECT_EQ(publication->publication_generation, 2U);
  EXPECT_EQ(publication->lifecycle_generation, 2U);
  EXPECT_EQ(
    publication->reason_codes,
    (std::vector<std::string>{
        "observation_stale_or_missing",
        "transport_not_connected"}));
}

TEST(AdmissionProjectorTest, AuthorityReplacementAdvancesGeneration) {
  auto projector = make_projector();
  ASSERT_TRUE(projector.project(ready_snapshot(), kNow).has_value());

  auto replacement = ready_snapshot();
  replacement.generation += 1;
  replacement.authority_session->session_id = "operator-session-2";
  replacement.authority_session->generation += 1;
  const auto publication =
    projector.project(replacement, kNow + 1);

  ASSERT_TRUE(publication.has_value());
  EXPECT_EQ(publication->lifecycle_generation, 2U);
}

TEST(AdmissionProjectorTest, RejectsRegressionsAndHiddenInvalidAdmission) {
  auto projector = make_projector();
  ASSERT_TRUE(projector.project(ready_snapshot(), kNow).has_value());

  auto regressed = ready_snapshot();
  regressed.generation -= 1;
  EXPECT_FALSE(projector.project(regressed, kNow + 1).has_value());

  auto invalid = ready_snapshot();
  invalid.generation += 1;
  invalid.controller_ready = false;
  EXPECT_FALSE(projector.project(invalid, kNow + 2).has_value());

  auto duplicate_reasons = ready_snapshot();
  duplicate_reasons.generation += 1;
  duplicate_reasons.motion_admitted = false;
  duplicate_reasons.admission_reason_codes = {"blocked", "blocked"};
  EXPECT_FALSE(
    projector.project(duplicate_reasons, kNow + 3).has_value());

  auto expired_deadline = ready_snapshot();
  expired_deadline.generation += 1;
  expired_deadline.observed.fresh_until_ns = kNow + 3;
  EXPECT_FALSE(
    projector.project(expired_deadline, kNow + 4).has_value());
}

}  // namespace
}  // namespace manyforge::robot_lifecycle
