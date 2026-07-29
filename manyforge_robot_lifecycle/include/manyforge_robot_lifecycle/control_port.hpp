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

#include <cstdint>
#include <string>
#include <string_view>

#include <manyforge_robot_lifecycle/contract.hpp>

namespace manyforge::robot_lifecycle
{

/// NRT command port implemented by exactly one robot-transport owner.
class LifecycleControlPort
{
public:
  virtual ~LifecycleControlPort() = default;

  LifecycleControlPort() = default;
  LifecycleControlPort(const LifecycleControlPort &) = delete;
  LifecycleControlPort & operator=(const LifecycleControlPort &) = delete;

  virtual bool establish_authority_session(
    const AuthoritySession & session,
    std::int64_t now_ns) = 0;
  virtual bool clear_authority_session(
    std::string_view session_id,
    std::uint64_t session_generation,
    std::int64_t now_ns) = 0;
  virtual CommandDecision submit_lifecycle(
    const CommandRequest & request,
    std::int64_t now_ns) = 0;
  virtual bool reconcile_lifecycle(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns) = 0;
  virtual bool refresh_lifecycle(std::int64_t now_ns) = 0;
  [[nodiscard]] virtual LifecycleSnapshot lifecycle_snapshot(
    std::int64_t now_ns) const = 0;
};

}  // namespace manyforge::robot_lifecycle
