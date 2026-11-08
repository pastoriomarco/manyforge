#pragma once

#include <manyforge_core/ports/iio_signal_service.hpp>
#include <manyforge_core/types/io_signal.hpp>

namespace manyforge::roboplan_backend::host_detail {

// Called only after the base output's owner, catalog and sealed descriptor
// have been qualified by the caller.
[[nodiscard]] inline core::Result<core::IoSignalObservationDTO> ReadBoundChangedOutside(
    const core::IIoSignalService &service, const core::IoSignalObservationDTO &base,
    const core::RequestOwnerEpoch &owner_epoch, std::uint64_t catalog_generation) {
  using Reply = core::Result<core::IoSignalObservationDTO>;
  if (!core::HoldsChangedOutside(base.descriptor))
    return Reply::Err({"invalid_request", "I/O signal request is invalid",
                       {{"reason", "io_signal_changed_outside_unavailable"}}});
  const auto derived = service.ReadSignal(core::ChangedOutsideSignalId(base.descriptor.signal_id));
  if (!derived.ok()) return Reply::Err(derived.error());
  if (derived.value().cursor.owner_epoch != owner_epoch ||
      derived.value().cursor.catalog_generation != catalog_generation)
    return Reply::Err({"io_signal_catalog_stale", "I/O service differs from current activation", {}});
  return derived;
}

} // namespace manyforge::roboplan_backend::host_detail
