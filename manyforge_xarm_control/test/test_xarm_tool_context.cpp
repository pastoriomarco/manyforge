// Copyright 2026 Marco Pastorio
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_xarm_control/xarm_tool_context.hpp>

namespace manyforge::xarm_control {
namespace {

using core::Pose3DTO;
using core::ToolContextPointMassDTO;

Pose3DTO Tcp(double z) { return {{0.0, 0.0, z}, {0.0, 0.0, 0.0, 1.0}}; }

struct Controller {
  std::vector<std::string> calls;
  std::vector<Pose3DTO> offsets;
  std::vector<ToolContextPointMassDTO> loads;
  std::optional<std::array<double, 6>> report;
  bool mismatch{false};
  bool nonfinite_ready{false};
  bool missing_ready_offset{false};
  bool offset_confirmed{true};
  XArmServiceCallOutcome load_outcome{XArmServiceCallOutcome::kSucceeded};
  std::string load_reason{"xarm_service_timeout"};
  std::optional<int> load_return;
  std::vector<XArmToolContextBound> bounds;

  XArmToolContextSender Sender() {
    return XArmToolContextSender(
        [this](const Pose3DTO &pose, const XArmToolContextBound &bound) {
          calls.emplace_back("offset");
          offsets.push_back(pose);
          bounds.push_back(bound);
          if (!offset_confirmed)
            return XArmTcpOffsetSent{false, core::ErrorDTO{
                "execution_rejected", "unconfirmed", {{"effect", "possible"}}}, {}};
          auto ready_offset = mismatch ? xarm_tcp_offset(Tcp(0.8)) : xarm_tcp_offset(pose);
          if (nonfinite_ready) ready_offset[0] = std::numeric_limits<double>::quiet_NaN();
          report = ready_offset;
          return XArmTcpOffsetSent{true, {}, missing_ready_offset
                                                   ? std::nullopt
                                                   : std::optional{ready_offset}};
        },
        [this](double mass, const std::array<double, 3> &cog,
               const XArmToolContextBound &bound) {
          calls.emplace_back("load");
          loads.push_back({mass, cog});
          bounds.push_back(bound);
          return XArmServiceCallResult{load_outcome,
              load_outcome == XArmServiceCallOutcome::kSucceeded ? "" : load_reason,
              load_return};
        },
        [this] { calls.emplace_back("report"); return report; });
  }
};

TEST(XArmToolContextSender, TcpChangeChecksTheReadyReportAndNoEventDoesNothing) {
  Controller c;
  auto sender = c.Sender();
  auto result = sender.Apply(Tcp(0.1), std::nullopt, false);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value());
  EXPECT_EQ(c.calls, (std::vector<std::string>{"offset"}));
  ASSERT_TRUE(sender.Apply(std::nullopt, std::nullopt, false).ok());
  EXPECT_EQ(c.calls.size(), 1U);
  ASSERT_TRUE(sender.Apply(Pose3DTO{}, std::nullopt, false).ok());
  EXPECT_EQ(c.offsets.back(), Pose3DTO{});
  c.mismatch = true;
  result = sender.Apply(Tcp(0.2), std::nullopt, false);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "tcp_offset_not_applied");
  EXPECT_EQ(result.error().details.at("reason"), "tcp_offset_not_applied");
  EXPECT_EQ(result.error().details.at("effect"), "observed");
}

TEST(XArmToolContextSender, NonfiniteReadyOffsetDoesNotConfirmTheSend) {
  Controller c;
  c.nonfinite_ready = true;
  auto sender = c.Sender();
  const auto result = sender.Apply(Tcp(0.1), std::nullopt, false);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "tcp_offset_not_applied");
  EXPECT_EQ(result.error().details.at("effect"), "observed");
}

TEST(XArmToolContextSender, MissingReadyOffsetDoesNotConfirmTheSend) {
  Controller c;
  c.missing_ready_offset = true;
  auto sender = c.Sender();
  const auto result = sender.Apply(Tcp(0.1), std::nullopt, false);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "tcp_offset_not_applied");
  EXPECT_EQ(result.error().details.at("effect"), "observed");
}

TEST(XArmToolContextSender, EnableResyncsBeforePayloadAndUsesZeroWithoutTcp) {
  Controller c;
  c.report = xarm_tcp_offset(Tcp(0.3));
  auto sender = c.Sender();
  const ToolContextPointMassDTO payload{1.0, {0.0, 0.0, 0.04}};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::stop_source stop;
  const auto result = sender.Apply(Pose3DTO{}, payload, true, {deadline, stop.get_token()});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(c.calls, (std::vector<std::string>{"report", "offset", "load"}));
  EXPECT_EQ(c.offsets.front(), Pose3DTO{});
  EXPECT_EQ(c.bounds[0].step_deadline, deadline);
  EXPECT_EQ(c.bounds[1].step_deadline, deadline);
  stop.request_stop();
  EXPECT_TRUE(c.bounds[0].cancellation.stop_requested());
  EXPECT_TRUE(c.bounds[1].cancellation.stop_requested());
  c.calls.clear();
  ASSERT_TRUE(sender.Apply(Pose3DTO{}, payload, true).ok());
  EXPECT_EQ(c.calls, (std::vector<std::string>{"report", "load"}));
  c.calls.clear();
  c.report.reset();
  const auto unavailable = sender.Apply(Tcp(0.1), payload, true);
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"), "xarm_status_unavailable");
  EXPECT_EQ(c.calls, (std::vector<std::string>{"report"}));
}

TEST(XArmToolContextSender, EnableAcceptsReportedOffsetWithinTolerance) {
  Controller c;
  c.report = xarm_tcp_offset(Tcp(0.1));
  (*c.report)[0] += 0.5 * kXArmTcpOffsetToleranceMm;
  (*c.report)[5] += 2.0 * std::numbers::pi;
  auto sender = c.Sender();
  ASSERT_TRUE(sender.Apply(Tcp(0.1), ToolContextPointMassDTO{}, true).ok());
  EXPECT_EQ(c.calls, (std::vector<std::string>{"report", "load"}));
}

TEST(XArmToolContextSender, KeepDoesNothingAndEnableZeroRestoresFlange) {
  Controller c;
  c.report = xarm_tcp_offset(Tcp(0.3));
  auto sender = c.Sender();
  ASSERT_TRUE(sender.Apply(std::nullopt, std::nullopt, false).ok());
  EXPECT_TRUE(c.calls.empty());
  ASSERT_TRUE(sender.Apply(Pose3DTO{}, ToolContextPointMassDTO{}, true).ok());
  EXPECT_EQ(c.calls, (std::vector<std::string>{"report", "offset", "load"}));
  EXPECT_EQ(c.offsets.front(), Pose3DTO{});
}

TEST(XArmToolContextSender, PayloadEventSendsOnlyLoadAndClassifiesUnknownEffect) {
  Controller c;
  auto sender = c.Sender();
  const ToolContextPointMassDTO payload{1.5, {0.0, 0.0, 0.08}};
  ASSERT_TRUE(sender.Apply(std::nullopt, payload, false).ok());
  EXPECT_EQ(c.calls, (std::vector<std::string>{"load"}));
  EXPECT_EQ(c.loads.front(), payload);
  c.load_outcome = XArmServiceCallOutcome::kUnknown;
  const auto unanswered = sender.Apply(std::nullopt, payload, false);
  ASSERT_FALSE(unanswered.ok());
  EXPECT_EQ(unanswered.error().code, "robot_controller_unresponsive");
  EXPECT_EQ(unanswered.error().details.at("effect"), "possible");
  c.load_reason = "xarm_service_returned_nonzero";
  c.load_return = 1;
  const auto nonzero = sender.Apply(std::nullopt, payload, false);
  ASSERT_FALSE(nonzero.ok());
  EXPECT_EQ(nonzero.error().details.at("effect"), "possible");
  EXPECT_EQ(nonzero.error().details.at("vendor_return"), "1");
  c.load_outcome = XArmServiceCallOutcome::kNotSent;
  const auto refused = sender.Apply(std::nullopt, payload, false);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.count("effect"), 0U);
}

TEST(XArmToolContextSender, FailedLoadAfterRearmReportsObservedEffect) {
  Controller c;
  c.report = xarm_tcp_offset(Tcp(0.3));
  c.load_outcome = XArmServiceCallOutcome::kNotSent;
  auto sender = c.Sender();
  const auto result = sender.Apply(Pose3DTO{}, ToolContextPointMassDTO{}, true);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("effect"), "observed");
  EXPECT_EQ(c.calls, (std::vector<std::string>{"report", "offset", "load"}));
}

TEST(XArmToolContextSender, FailedOffsetDoesNotSendPayload) {
  Controller c;
  c.offset_confirmed = false;
  auto sender = c.Sender();
  const auto result = sender.Apply(Tcp(0.1), ToolContextPointMassDTO{}, false);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("effect"), "possible");
  EXPECT_EQ(c.calls, (std::vector<std::string>{"offset"}));
}

} // namespace
} // namespace manyforge::xarm_control
