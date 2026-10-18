#pragma once

#include <manyforge_core/types/io_blackboard.hpp>
#include <manyforge_core/types/program_io_condition.hpp>
#include <memory>

namespace manyforge::core {

/// Trusted host product derived from actual named-Apply publication, successful
/// prepared receipt and sealed Program/Deployment compiler products. It carries
/// no filesystem callback and is never accepted from the public BB packet.
/// Installation retains the existing composition/admission guards throughout
/// the exact Kernel service/configuration/current-owner recheck and commit.
struct AppliedProgramBlackboardContextDTO final {
  RequestOwnerEpoch request_owner_epoch;
  ActiveBundlePublicationGeneration active_publication_generation{0U};
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::shared_ptr<const ProgramBindingDTO> program_binding;
  std::shared_ptr<const std::vector<ProgramBlackboardKeyDeclarationDTO>>
      blackboard_key_declarations;
  std::shared_ptr<const ProgramIoConditionProjectionDTO> io_conditions;
  /// Exact fixed referenced-key union from both actual sealed compilers, with
  /// scalar types resolved from the above declaration authority.
  std::vector<IoBlackboardKeyDescriptorDTO> required_keys;
  /// Server-derived established transport capacity, never a public knob.
  std::size_t maximum_value_bytes;
};

}  // namespace manyforge::core
