#pragma once

#include <cstddef>

namespace manyforge::deployment {

/// Maximum canonical encoded size of one activation-preparation request.
///
/// This operation carries a compressed model-source closure and therefore has
/// a dedicated transport bound.  It must not inherit or enlarge the generic
/// stored-artifact limit.
inline constexpr std::size_t kMaxActivationPreparationRequestBytes =
    34U * 1024U * 1024U;

} // namespace manyforge::deployment
