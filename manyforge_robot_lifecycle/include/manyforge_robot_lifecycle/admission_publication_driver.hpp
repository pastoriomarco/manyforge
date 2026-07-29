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
#include <functional>

#include <manyforge_robot_lifecycle/admission_projector.hpp>
#include <manyforge_robot_lifecycle/control_port.hpp>

namespace manyforge::robot_lifecycle
{

struct AdmissionPublicationResult
{
  bool refresh_succeeded = false;
  bool projection_succeeded = false;
  bool sink_succeeded = false;
};

/// NRT coordinator for one lifecycle owner and one admission publication sink.
///
/// A failed provider refresh is still followed by snapshot projection. This
/// lets the provider publish an explicit closed state when its control port
/// can represent the failure; otherwise the prior consumer lease simply
/// expires.
class AdmissionPublicationDriver final
{
public:
  using PublicationSink =
    std::function<bool(const AdmissionPublication &)>;

  AdmissionPublicationDriver(
    LifecycleControlPort & control_port,
    AdmissionProjectorConfig projector_config,
    PublicationSink sink);

  AdmissionPublicationDriver(
    const AdmissionPublicationDriver &) = delete;
  AdmissionPublicationDriver & operator=(
    const AdmissionPublicationDriver &) = delete;

  [[nodiscard]] AdmissionPublicationResult tick(std::int64_t now_ns);

private:
  LifecycleControlPort & control_port_;
  AdmissionProjector projector_;
  PublicationSink sink_;
};

}  // namespace manyforge::robot_lifecycle
