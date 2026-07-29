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

#pragma once

#include <vector>

#include <manyforge_xarm_control/contract.hpp>

namespace manyforge::xarm_control
{

/// Separately addressable Lite6 lifecycle transitions.
///
/// Every entry delegates exactly one vendor primitive. Composite convenience
/// actions belong above this catalog so partial effects remain attributable.
std::vector<XArmActionPlan> make_lite6_xarm_action_plans();
std::vector<XArmTargetPlan> make_lite6_xarm_target_plans();

}  // namespace manyforge::xarm_control
