#include "host_io_signal_runtime.hpp"
#include "ros_host_composition.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

namespace manyforge::roboplan_backend::host_detail {
namespace {

class HostRuleEvidenceWakeupTest : public ::testing::Test {
protected:
  void SetUp() override {
    // A real BB-only service needs no physical provider or invented sample.
    runtime = HostIoSignalRuntime::Create(
        core::RequestOwnerEpoch("host"), 1U, {}, clock, scheduler, 1U);
  }
  void Advance() {
    scheduler->advance_to(clock->now(), core::SchedulerPriority::RuntimeSafety);
  }
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<core::ManualScheduler> scheduler =
      std::make_shared<core::ManualScheduler>();
  std::shared_ptr<HostIoSignalRuntime> runtime;
};

TEST_F(HostRuleEvidenceWakeupTest, CoalescesWakeAndRunsWithoutRuntimeLock) {
  unsigned calls{};
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    // These methods take the runtime mutex. The scheduler callback must have
    // released it before entering its actual owner's processing function.
    EXPECT_TRUE(runtime->has_rule_evidence_processor());
    EXPECT_TRUE(runtime->RuleServiceConfiguration().ok());
    return core::Result<bool>::Ok(true);
  });
  runtime->RequestRuleEvidenceProcessing();
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
  runtime->Start();
  runtime->RequestRuleEvidenceProcessing();
  runtime->RequestRuleEvidenceProcessing();
  EXPECT_EQ(calls, 0U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 1U);
  Advance();
  EXPECT_EQ(calls, 1U);
  // This service has no armed expiring source. There is no periodic poller.
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
}

TEST_F(HostRuleEvidenceWakeupTest, OriginalProcessingErrorDoesNotScheduleRetry) {
  unsigned calls{};
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    return core::Result<bool>::Err(
        {"persistence_degraded", "actual owner error", {{"reason", "test"}}});
  });
  runtime->Start();
  testing::internal::CaptureStderr();
  Advance();
  const auto diagnostic = testing::internal::GetCapturedStderr();
  EXPECT_NE(diagnostic.find("persistence_degraded actual owner error"),
            std::string::npos);
  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
  Advance();
  EXPECT_EQ(calls, 1U);
}

TEST_F(HostRuleEvidenceWakeupTest, DetachAndShutdownCancelQueuedWork) {
  unsigned calls{};
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    return core::Result<bool>::Ok(true);
  });
  runtime->Start();
  runtime->DetachRuleEvidenceProcessor();
  EXPECT_FALSE(runtime->has_rule_evidence_processor());
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
  Advance();
  EXPECT_EQ(calls, 0U);
  runtime->SetRuleEvidenceProcessor([&] {
    ++calls;
    return core::Result<bool>::Ok(true);
  });
  runtime->RequestRuleEvidenceProcessing();
  runtime->Shutdown();
  Advance();
  EXPECT_EQ(calls, 0U);
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
}

TEST_F(HostRuleEvidenceWakeupTest, CallbackBorrowsActualAccessWithoutRetainingIt) {
  auto access = std::make_shared<ExecutionCompositionAccess>();
  std::weak_ptr<ExecutionCompositionAccess> weak = access;
  // Only the callback attachment is under test; no selected product or
  // publication authority is supplied or inferred by this helper.
  ExecutionProduct product(nullptr, nullptr, nullptr, {},
                           core::ActiveBundlePublicationGeneration(1U));
  product.io_signal_runtime = runtime;
  BindHostIoRuleEvidenceProcessor(product, access);
  runtime->Start();
  // No Kernel is installed in this access object. A queued callback cannot
  // invent one, and the callback's weak pin cannot extend its lifetime.
  access->detach_addressed_owner();
  access.reset();
  EXPECT_TRUE(weak.expired());
  Advance();
  EXPECT_EQ(scheduler->pending_count(core::SchedulerPriority::RuntimeSafety), 0U);
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
