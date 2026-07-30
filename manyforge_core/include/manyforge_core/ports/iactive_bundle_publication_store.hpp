#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>

namespace manyforge::core {

/// Scene-owner capability for independent durable publication verification.
///
/// This capability intentionally cannot publish.
class IActiveBundlePublicationReader {
 public:
  virtual ~IActiveBundlePublicationReader() = default;

  [[nodiscard]] virtual Result<ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      ActiveBundlePublicationGeneration publication_generation) const = 0;
};

/// Manager capability and durable owner of the public active-bundle pointer.
///
/// The Manager can publish and may retain the read capability for replay and
/// diagnostics. Scene finalization is given only the base reader capability.
class IActiveBundlePublicationStore : public IActiveBundlePublicationReader {
 public:
  ~IActiveBundlePublicationStore() override = default;

  [[nodiscard]] virtual ActiveBundlePublicationReceiptDTO PublishActiveBundle(
      const ActiveBundlePublicationRequestDTO& request) = 0;
};

}  // namespace manyforge::core
