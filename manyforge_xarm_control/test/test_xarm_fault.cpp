// Copyright 2026 Marco Pastorio
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

// The plan's fault catalogue for xArm controller codes.

#include <gtest/gtest.h>

#include <manyforge_xarm_control/xarm_fault.hpp>

namespace manyforge::xarm_control {
namespace {

using core::FaultCode;
using core::FaultOperatorAction;

TEST(XArmFault, CatalogueCodesMapToTheirFaultAndKeepTheVendorCode) {
  const struct {
    int code;
    FaultCode fault;
    FaultOperatorAction action;
  } rows[] = {
      {1, FaultCode::kEstop, FaultOperatorAction::kRecover},
      {2, FaultCode::kEstop, FaultOperatorAction::kRecover},
      {3, FaultCode::kEstop, FaultOperatorAction::kRecover},
      {31, FaultCode::kProtectiveStop, FaultOperatorAction::kRecover},
      {23, FaultCode::kLimitViolation, FaultOperatorAction::kRecover},
      {24, FaultCode::kLimitViolation, FaultOperatorAction::kRecover},
      {35, FaultCode::kLimitViolation, FaultOperatorAction::kRecover},
      {60, FaultCode::kLimitViolation, FaultOperatorAction::kRecover},
      {22, FaultCode::kSelfCollision, FaultOperatorAction::kRecover},
      {21, FaultCode::kMotionInvalid, FaultOperatorAction::kRecover},
      {25, FaultCode::kMotionInvalid, FaultOperatorAction::kRecover},
      {10, FaultCode::kDriveFault, FaultOperatorAction::kNone},
      {17, FaultCode::kDriveFault, FaultOperatorAction::kNone},
      {19, FaultCode::kCommunication, FaultOperatorAction::kRecover},
      {33, FaultCode::kCommunication, FaultOperatorAction::kRecover},
  };
  for (const auto &row : rows) {
    const auto fault = xarm_fault(row.code, 42);
    ASSERT_TRUE(fault.has_value()) << row.code;
    EXPECT_EQ(fault->code, row.fault) << row.code;
    EXPECT_EQ(fault->operator_action, row.action) << row.code;
    EXPECT_EQ(fault->clearable, row.fault != FaultCode::kDriveFault) << row.code;
    EXPECT_EQ(fault->vendor_code, "C" + std::to_string(row.code));
    EXPECT_EQ(fault->vendor_text, std::string(xarm_error_text(row.code)));
    EXPECT_EQ(fault->since_ns, 42);
  }
  EXPECT_EQ(xarm_fault(2, 0)->vendor_text, "Emergency IO of Control Box is triggered");
}

TEST(XArmFault, UnmappedCodesPassThroughAsVendorFaultAndZeroIsNoFault) {
  const auto fault = xarm_fault(26, 0);
  ASSERT_TRUE(fault.has_value());
  EXPECT_EQ(fault->code, FaultCode::kVendorFault);
  EXPECT_EQ(fault->vendor_code, "C26");
  EXPECT_EQ(fault->vendor_text, "System Real Time Error");
  EXPECT_EQ(xarm_fault(999, 0)->code, FaultCode::kVendorFault);
  EXPECT_FALSE(xarm_fault(0, 0).has_value());
  EXPECT_FALSE(xarm_fault(-1, 0).has_value());
}

TEST(XArmFault, EachCodeConcernsItsPartOfTheMachine) {
  EXPECT_EQ(xarm_fault_scope(33), XArmFaultScope::kControllerIo);
  EXPECT_EQ(xarm_fault_scope(19), XArmFaultScope::kToolIo);
  for (const int code : {10, 17, 21, 22, 23, 24, 25, 31, 35, 60})
    EXPECT_EQ(xarm_fault_scope(code), XArmFaultScope::kArm) << code;
  for (const int code : {1, 2, 3, 26, 999})
    EXPECT_EQ(xarm_fault_scope(code), XArmFaultScope::kMachine) << code;
}

TEST(XArmFault, ExternalControlIsClearableByEnable) {
  const auto fault = xarm_external_control_fault(0, 5, 7);
  EXPECT_EQ(fault.code, FaultCode::kExternalControl);
  EXPECT_TRUE(fault.clearable);
  EXPECT_EQ(fault.operator_action, FaultOperatorAction::kRecover);
  EXPECT_EQ(fault.vendor_code, "mode=0 state=5");
  EXPECT_EQ(fault.since_ns, 7);
}

} // namespace
} // namespace manyforge::xarm_control
