#pragma once

#include <filesystem>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <vector>

namespace manyforge::roboplan_backend {

/// Exact point resolver used by the native execution owner.
///
/// Programs and generated Program Bindings are loaded by the identities in
/// each bounded operation request. The resolved semantic binding must remain
/// inside one of the already-composed physical participant inventories; this
/// object performs no repository enumeration, latest selection, or backend
/// discovery.
class ProductBackedRuntimeParticipantResolver final {
 public:
  ProductBackedRuntimeParticipantResolver(
      std::filesystem::path package_root,
      std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
      deployment::VerifiedRetainedModelMap retained_model_map,
      std::vector<core::RuntimeParticipantBinding> physical_bindings);

  [[nodiscard]] core::Result<core::RuntimeParticipantBinding> Resolve(
      const core::OperationIntentDTO& operation,
      std::size_t participant_index) const;

  ProductBackedRuntimeParticipantResolver(
      const ProductBackedRuntimeParticipantResolver&) = delete;
  ProductBackedRuntimeParticipantResolver& operator=(
      const ProductBackedRuntimeParticipantResolver&) = delete;

 private:
  deployment::FilesystemProgramRepository programs_;
  deployment::FilesystemProgramBindingRepository bindings_;
  std::shared_ptr<const deployment::RetainedBundle> retained_bundle_;
  deployment::VerifiedRetainedModelMap retained_model_map_;
  std::vector<core::RuntimeParticipantBinding> physical_bindings_;
};

}  // namespace manyforge::roboplan_backend
