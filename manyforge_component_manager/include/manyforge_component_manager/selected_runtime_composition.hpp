#pragma once

#include <cstdint>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/projection_apply.hpp>

namespace manyforge::component_manager {

/// Exact Manager-to-managed-host request to install the already selected
/// product composition while its durable Kernel configuration fence is held.
/// This is a private Apply effect, not caller authorization or a command
/// session.
struct ManagerSelectedRuntimeCompositionRequestDTO final {
  manyforge::core::ContentDigest scene_activated_digest;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ProjectionStageGeneration projection_stage_generation;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ActiveBundlePublicationGeneration
      expected_predecessor_publication_generation;
  manyforge::core::ContentDigest expected_predecessor_public_pointer_digest;
  manyforge::core::ContentDigest retained_bundle_canonical_content_digest;
  std::uint64_t retained_bundle_canonical_byte_size{};
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;
  manyforge::core::ConfigurationFenceId configuration_fence_id;
  manyforge::core::ConfigurationFenceGeneration configuration_fence_generation;
  manyforge::core::ConfigurationFenceScopeDTO configuration_fence_scope;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedRuntimeCompositionRequestDTO &,
             const ManagerSelectedRuntimeCompositionRequestDTO &) = default;
};

enum class ManagerSelectedRuntimeCompositionState {
  kPreparing,
  kInstalled,
};

struct ManagerSelectedRuntimeCompositionResultDTO final {
  ManagerSelectedRuntimeCompositionRequestDTO request;
  ManagerSelectedRuntimeCompositionState state;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedRuntimeCompositionResultDTO &,
             const ManagerSelectedRuntimeCompositionResultDTO &) = default;
};

class IManagerSelectedRuntimeCompositionPort {
public:
  virtual ~IManagerSelectedRuntimeCompositionPort() = default;

  [[nodiscard]] virtual ManagerSelectedRuntimeCompositionResultDTO
  InstallOrObserve(
      const ManagerSelectedRuntimeCompositionRequestDTO &request) = 0;
};

} // namespace manyforge::component_manager
