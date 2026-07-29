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
#include <optional>
#include <string>

#include <manyforge_robot_lifecycle/admission_publication_driver.hpp>

namespace manyforge::robot_lifecycle
{
namespace
{

constexpr std::int64_t kNow = 1'000'000'000;

class FakeControlPort final : public LifecycleControlPort
{
public:
  bool establish_authority_session(
    const AuthoritySession &, std::int64_t) override
  {
    return false;
  }

  bool clear_authority_session(
    std::string_view, std::uint64_t, std::int64_t) override
  {
    return false;
  }

  CommandDecision submit_lifecycle(
    const CommandRequest &, std::int64_t) override
  {
    return {};
  }

  bool reconcile_lifecycle(
    std::uint64_t, std::string_view, std::string,
    std::int64_t) override
  {
    return false;
  }

  bool refresh_lifecycle(std::int64_t) override
  {
    ++refresh_count;
    return refresh_result;
  }

  LifecycleSnapshot lifecycle_snapshot(std::int64_t) const override
  {
    return snapshot;
  }

  bool refresh_result = true;
  std::size_t refresh_count = 0;
  LifecycleSnapshot snapshot;
};

LifecycleSnapshot closed_snapshot()
{
  LifecycleSnapshot snapshot;
  snapshot.robot_id = "lite6-a";
  snapshot.supervisor_instance_id = "supervisor-instance-1";
  snapshot.generation = 1;
  snapshot.admission_reason_codes = {
    "authority_session_stale_or_missing",
    "observation_stale_or_missing",
  };
  return snapshot;
}

TEST(AdmissionPublicationDriverTest, PublishesClosedStateAfterRefreshFailure) {
  FakeControlPort port;
  port.refresh_result = false;
  port.snapshot = closed_snapshot();
  std::optional<AdmissionPublication> captured;
  AdmissionPublicationDriver driver(
    port,
    AdmissionProjectorConfig{
        "provider-instance-1",
        "lite6-a",
        "binding-generation-1",
        250'000'000,
        "runtime-generation-1",
        "provider-epoch-1",
        "supervisor-epoch-1"},
    [&captured](const AdmissionPublication & publication) {
      captured = publication;
      return true;
    });

  const auto result = driver.tick(kNow);

  EXPECT_FALSE(result.refresh_succeeded);
  EXPECT_TRUE(result.projection_succeeded);
  EXPECT_TRUE(result.sink_succeeded);
  ASSERT_TRUE(captured.has_value());
  EXPECT_FALSE(captured->motion_admitted);
  EXPECT_EQ(port.refresh_count, 1U);
}

TEST(AdmissionPublicationDriverTest, ReportsSinkRejection) {
  FakeControlPort port;
  port.snapshot = closed_snapshot();
  AdmissionPublicationDriver driver(
    port,
    AdmissionProjectorConfig{
        "provider-instance-1",
        "lite6-a",
        "binding-generation-1",
        250'000'000,
        "runtime-generation-1",
        "provider-epoch-1",
        "supervisor-epoch-1"},
    [](const AdmissionPublication &) {return false;});

  const auto result = driver.tick(kNow);

  EXPECT_TRUE(result.refresh_succeeded);
  EXPECT_TRUE(result.projection_succeeded);
  EXPECT_FALSE(result.sink_succeeded);
}

}  // namespace
}  // namespace manyforge::robot_lifecycle
