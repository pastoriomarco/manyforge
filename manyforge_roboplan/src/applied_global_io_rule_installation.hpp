#pragma once

#include <filesystem>

#include <manyforge_core/ports/iio_signal_service.hpp>
#include <manyforge_core/types/io_activation_rule.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::core {
class ResourceAddressedExecutionKernel;
}

namespace manyforge::roboplan_backend::host_detail {

struct ExecutionCompositionAccess;

// Exactly one immutable candidate product, or neither for qualified empty.
// Both alternatives share the existing prepared/Applied product lifetime.
struct CompiledGlobalIoRuleProduct final {
  std::shared_ptr<const core::IoActivationRuleCatalogDTO> catalog{};
  std::shared_ptr<const core::IoRuleWaitingForProgramCatalogDTO>
      waiting_for_program{};
};

// Pure sealed candidate projection plus actual service identity when present.
// Without a service, only an entirely waiting no-Program rule set or an empty
// reference union is permitted. This grants no successful Apply/source facts.
[[nodiscard]] core::Result<CompiledGlobalIoRuleProduct>
CompileGlobalIoRuleCatalog(
    const core::IoRuleAppliedContextDTO &context,
    const deployment::RetainedBundle &bundle,
    const deployment::ProgramBindingContract *program,
    const deployment::ProgramBinding *binding,
    const std::shared_ptr<core::IIoSignalService> &service);

// Activates the installed product's rules once the caller holds the Manager's
// success of its Apply; the composition pin is held through the Core gate.
[[nodiscard]] core::Result<void> EnsureAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel);

// The same, for the startup recovery exclusion. Core independently requires
// route_recovery_pending at commit.
[[nodiscard]] core::Result<void> RestoreAppliedGlobalIoRuleCatalog(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel);

} // namespace manyforge::roboplan_backend::host_detail
