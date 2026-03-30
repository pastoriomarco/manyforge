#pragma once

namespace manyforge::core {

/// Tolerances DTO (spec §3.2)
struct TolerancesDTO {
    double joint_position_tol_rad;  // >= 0
    double translation_tol_m;       // >= 0
    double rotation_tol_rad;        // >= 0
};

}  // namespace manyforge::core
