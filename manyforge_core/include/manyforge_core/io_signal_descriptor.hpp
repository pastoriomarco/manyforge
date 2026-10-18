#pragma once

#include <manyforge_core/types/io_signal.hpp>

namespace manyforge::core {

/// One canonical digest shared by retention, bindings and the installed host
/// catalog. Provider transport addresses remain in sealed runtime configuration.
[[nodiscard]] Result<ContentDigest> ComputeIoSignalDescriptorDigest(
    const IoSignalDescriptorDTO& descriptor);

}  // namespace manyforge::core
