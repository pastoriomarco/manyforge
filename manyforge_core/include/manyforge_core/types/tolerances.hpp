#pragma once

namespace manyforge::core {

/// Tolerances DTO
struct TolerancesDTO {
    double joint_position_tol_rad;  // >= 0
    double translation_tol_m;       // >= 0
    double rotation_tol_rad;        // >= 0
    friend bool operator==(const TolerancesDTO&, const TolerancesDTO&) = default;
};

}  // namespace manyforge::core
