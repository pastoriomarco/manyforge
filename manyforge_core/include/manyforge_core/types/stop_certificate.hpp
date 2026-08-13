#pragma once

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Compact, exact reference to one immutable Kernel Stop certificate.
///
/// The certificate remains owned and queryable through the Kernel. Other
/// bounded status families carry this value rather than repeatedly copying
/// the complete certificate and its evidence arrays.
struct StopCertificateRefDTO final {
  StopCertificateId stop_certificate_id;
  StopCertificateRevision stop_certificate_revision;
  StopId stop_id;
  StopGeneration stop_generation;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !stop_certificate_id.empty() && stop_certificate_revision.nonzero() &&
           !stop_id.empty() && stop_generation.nonzero();
  }

  friend bool operator==(const StopCertificateRefDTO&, const StopCertificateRefDTO&) = default;
};

}  // namespace manyforge::core
