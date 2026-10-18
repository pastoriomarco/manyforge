#pragma once

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Portable authored selection and value. The existing Kernel resolves the
/// actual output resource, provider/channel, domain and transport from seals.
struct IoSignalWriteCommandValueDTO final {
  ResourceRoleId resource_role_id;
  bool value;

  friend bool operator==(const IoSignalWriteCommandValueDTO&,
                         const IoSignalWriteCommandValueDTO&) = default;
};

}  // namespace manyforge::core
