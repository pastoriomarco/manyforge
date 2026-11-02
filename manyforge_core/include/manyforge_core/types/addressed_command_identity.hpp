#pragma once

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Exact identity of one committed addressed command, supplied by the existing
/// operation owner. It neither admits a command nor proves transport effects.
struct AddressedCommandIdentityDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllerGoalId command_id;
  ControllerGoalGeneration command_generation;

  friend bool operator==(const AddressedCommandIdentityDTO&,
                         const AddressedCommandIdentityDTO&) = default;
};

}  // namespace manyforge::core
