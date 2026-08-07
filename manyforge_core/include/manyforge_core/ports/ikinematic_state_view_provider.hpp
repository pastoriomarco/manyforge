#pragma once

#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/kinematic_state_view.hpp>

namespace manyforge::core {

class IKinematicStateViewProvider {
 public:
  virtual ~IKinematicStateViewProvider() = default;

  [[nodiscard]] virtual Result<KinematicStatePathViewDTO> current_kinematic_state_path(
      const KinematicStatePathQueryDTO& query, std::int64_t now_ns) const = 0;
};

}  // namespace manyforge::core
