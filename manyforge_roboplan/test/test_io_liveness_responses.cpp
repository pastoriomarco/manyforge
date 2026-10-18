#include "execution_host_responses.hpp"
#include <gtest/gtest.h>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>

namespace manyforge::roboplan_backend::host_detail {
namespace {
TEST(IoLivenessResponseTest,
     DescriptorModeIsVisibleBeforeFirstSampleWithoutReadinessInference) {
  for (const auto mode : {core::IoSignalLivenessMode::kInProcess,
                          core::IoSignalLivenessMode::kPeriodic,
                          core::IoSignalLivenessMode::kChangeOnly}) {
    core::FakeClock clock;
    const core::InterruptSourceId source("source.io");
    const core::IoSignalId id{core::ControllableComponentId("component.io"),
                              "input"};
    const bool periodic = mode == core::IoSignalLivenessMode::kPeriodic;
    core::IoSignalDescriptorDTO descriptor{
        id,
        source,
        core::IoSignalValueType::kBoolean,
        core::IoSignalDirection::kInput,
        periodic ? core::IoSignalUpdateMode::kSampled
                 : core::IoSignalUpdateMode::kChangeOnly,
        core::IoTransitionCapability::kSampledOnly,
        {periodic ? core::IoFreshnessMode::kSampleAge
                  : core::IoFreshnessMode::kProviderValidity,
         periodic ? std::optional<core::Duration>{100} : std::nullopt},
        std::nullopt,
        mode};
    core::IoSignalCatalogDTO catalog{
        core::RequestOwnerEpoch("host.1"), 1U, {descriptor}, {}};
    if (mode == core::IoSignalLivenessMode::kInProcess)
      catalog.authoritative_synthetic_sources.push_back(source);
    auto created = core::IoSignalSubsystem::Create(
        catalog, {1, 1, 8, 8, 512, 1024}, clock);
    ASSERT_TRUE(created.ok());
    const auto observed = created.value()->ReadSignal(id);
    ASSERT_TRUE(observed.ok());
    const auto response = IoSignalObservationValue(observed.value());
    EXPECT_TRUE(response.find_object_member("value")->is_null());
    const auto *liveness = response.find_object_member("liveness");
    ASSERT_NE(liveness, nullptr);
    EXPECT_EQ(liveness->find_object_member("mode")->as_string(),
              periodic                                         ? "periodic"
              : mode == core::IoSignalLivenessMode::kInProcess ? "in_process"
                                                               : "change_only");
    const auto *warning = liveness->find_object_member("warning");
    ASSERT_NE(warning, nullptr);
    if (mode == core::IoSignalLivenessMode::kChangeOnly)
      EXPECT_EQ(warning->as_string(), "liveness_unproven");
    else
      EXPECT_TRUE(warning->is_null());
    EXPECT_FALSE(observed.value().latest_sample);
  }
}
TEST(IoLivenessResponseTest, OutputHasNoInputLivenessDeclaration) {
  core::FakeClock clock;
  const core::IoSignalId id{core::ControllableComponentId("component.io"),
                            "output"};
  core::IoSignalDescriptorDTO descriptor{
      id,
      core::InterruptSourceId("source.io"),
      core::IoSignalValueType::kBoolean,
      core::IoSignalDirection::kOutput,
      core::IoSignalUpdateMode::kSampled,
      core::IoTransitionCapability::kSampledOnly,
      {core::IoFreshnessMode::kSampleAge, 100},
      core::ResourceId("resource.output"),
      std::nullopt};
  auto created = core::IoSignalSubsystem::Create(
      {core::RequestOwnerEpoch("host.1"), 1U, {descriptor}, {}},
      {1, 1, 8, 8, 512, 1024}, clock);
  ASSERT_TRUE(created.ok());
  const auto observed = created.value()->ReadSignal(id);
  ASSERT_TRUE(observed.ok());
  EXPECT_TRUE(IoSignalObservationValue(observed.value())
                  .find_object_member("liveness")
                  ->is_null());
}
} // namespace
} // namespace manyforge::roboplan_backend::host_detail
