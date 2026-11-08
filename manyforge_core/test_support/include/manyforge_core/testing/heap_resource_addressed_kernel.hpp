#pragma once

#include <manyforge_core/resource_addressed_execution_kernel.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace manyforge::core::testing {

[[nodiscard]] inline std::unique_ptr<ResourceAddressedExecutionKernel>
make_heap_resource_addressed_kernel(
    std::vector<RuntimeParticipantExecutionRegistration> registrations,
    IClock& clock, IScheduler& /*scheduler*/, IOperationJournal& journal,
    ResourceAddressedExecutionKernelConfig config) {
  return std::make_unique<ResourceAddressedExecutionKernel>(
      std::move(registrations), clock, journal, std::move(config));
}

}  // namespace manyforge::core::testing
