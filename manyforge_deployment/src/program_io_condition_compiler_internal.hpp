#pragma once

#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::deployment::detail {

using ProgramIoSignalReference = core::ProgramSignalReferenceDTO;

/// Dependency discovery only: the same literal node/condition traversal as the
/// full compiler, before a Binding exists. Returns no types, signal identities,
/// descriptors or checked condition; actual binding/qualification remains
/// required.
[[nodiscard]] core::Result<std::vector<ProgramIoSignalReference>>
collect_program_node_io_signal_references(const CanonicalValue &node);

struct CompiledProgramIoCondition final {
  std::shared_ptr<const core::CheckedIoCondition> condition;
  std::vector<core::InterruptSourceId> required_signal_sources;
  /// Nonempty only for the explicit no-Program global compile mode. Then
  /// condition is null: no unresolved key kind or partial condition is exposed.
  std::vector<std::string> waiting_for_program_keys{};
};

/// Check one authored comparison against an already-qualified retained signal.
/// This grants no binding or runtime authority; direction/liveness are checked
/// by the caller. Uses the same literal parsing and Core checker as lowering.
[[nodiscard]] core::Result<void> check_program_signal_comparison(
    const core::IoSignalDescriptorDTO &descriptor, const CanonicalValue &op,
    const CanonicalValue &value);

/// Mechanical lowering only. The public compiler supplies all three views
/// from its exact sealed inputs; this internal helper installs no authority.
[[nodiscard]] core::Result<CompiledProgramIoCondition>
compile_authored_program_io_condition(
    const CanonicalValue &condition,
    std::span<const core::ProgramSignalBindingDTO> bindings,
    std::span<const core::ProgramBlackboardKeyDeclarationDTO> keys,
    std::span<const RetainedIoSignalConfiguration> signals);

[[nodiscard]] core::Result<CompiledProgramIoCondition>
compile_authored_deployment_io_condition(
    const CanonicalValue &condition,
    std::span<const core::ProgramBlackboardKeyDeclarationDTO> keys,
    std::span<const RetainedIoSignalConfiguration> signals,
    bool allow_waiting_program_keys = false);

} // namespace manyforge::deployment::detail
