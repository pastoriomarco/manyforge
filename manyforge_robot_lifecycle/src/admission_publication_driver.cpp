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

#include <manyforge_robot_lifecycle/admission_publication_driver.hpp>

#include <stdexcept>
#include <utility>

namespace manyforge::robot_lifecycle
{

AdmissionPublicationDriver::AdmissionPublicationDriver(
  LifecycleControlPort & control_port,
  AdmissionProjectorConfig projector_config,
  PublicationSink sink)
: control_port_(control_port),
  projector_(std::move(projector_config)),
  sink_(std::move(sink))
{
  if (!sink_) {
    throw std::invalid_argument(
            "admission publication sink is required");
  }
}

AdmissionPublicationResult AdmissionPublicationDriver::tick(
  std::int64_t now_ns)
{
  AdmissionPublicationResult result;
  result.refresh_succeeded =
    control_port_.refresh_lifecycle(now_ns);
  auto publication = projector_.project(
    control_port_.lifecycle_snapshot(now_ns), now_ns);
  if (!publication.has_value()) {
    return result;
  }
  result.projection_succeeded = true;
  result.sink_succeeded = sink_(*publication);
  return result;
}

}  // namespace manyforge::robot_lifecycle
