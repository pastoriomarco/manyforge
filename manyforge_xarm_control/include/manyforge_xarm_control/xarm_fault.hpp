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

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <manyforge_core/types/component_state.hpp>

namespace manyforge::xarm_control {

/// The controller's own text for an error code (the driver's
/// `controller_error_interpreter`).
[[nodiscard]] std::string_view xarm_error_text(int code) noexcept;

/// `C<code>`, the vendor code as UFactory writes it.
[[nodiscard]] std::string xarm_vendor_code(int code);

/// Servo errors C10-C17 need a power cycle; their error is not cleared.
[[nodiscard]] bool xarm_drive_error(int code) noexcept;

/// The plan's fault catalogue for a controller error code (CONTRACTS §4), as
/// the machine owner's fault (its operator action is the owner's Recover):
/// C1-C3 `fault_estop`, C31 `fault_protective_stop`, C23/C24/C35/C60
/// `fault_limit_violation`, C22 `fault_self_collision`, C21/C25
/// `fault_motion_invalid`, C10-C17 `fault_drive` (not clearable, power cycle),
/// C19/C33 `fault_communication`, anything else `fault_vendor`; each with the
/// vendor code and text. None for code 0 (no error) or a negative code.
[[nodiscard]] std::optional<core::Fault> xarm_fault(int code, std::int64_t since_ns);

/// Which part of the machine a controller error concerns (CONTRACTS §4, §16):
/// C19 the tool I/O, C33 the controller I/O, the arm's own errors (C10-C17
/// servo, C21/C25 motion, C22 self-collision, C23/C24/C35/C60 limits, C31
/// collision) the arm, anything else (e-stops C1-C3 included) every member.
enum class XArmFaultScope : std::uint8_t { kMachine, kArm, kControllerIo, kToolIo };
[[nodiscard]] XArmFaultScope xarm_fault_scope(int code) noexcept;

/// A mode or state change from outside ManyForge (UFactory Studio, the
/// pendant) while ManyForge has the arm armed: the machine owner's
/// `fault_external_control` (CONTRACTS §16, operator action `recover`); the
/// owner's Recover releases the arm and its Enable takes control back.
[[nodiscard]] core::Fault xarm_external_control_fault(int mode, int state,
                                                      std::int64_t since_ns);

} // namespace manyforge::xarm_control
