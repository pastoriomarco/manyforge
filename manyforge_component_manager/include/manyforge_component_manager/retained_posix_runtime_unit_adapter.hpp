#pragma once

#include <filesystem>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace manyforge::deployment {
class IVerifiedRetainedBundleSource;
class IVerifiedRetainedProductArtifactSource;
} // namespace manyforge::deployment

namespace manyforge::component_manager {

inline constexpr std::string_view kRetainedPosixProcessAdapterContractId =
    "manyforge.posix_sh.retained_script";

class RetainedPosixRuntimeUnitAdapterError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Concrete closed adapter for the one I3-qualified retained POSIX reference
/// runtime product.
///
/// The constructor accepts disjoint trusted, existing execution state and run
/// roots plus read-only verified retained-product sources. Durable controller
/// and process identities live only in state; locks, sockets, and verified
/// launch material live only in run. Runtime calls accept only the exact
/// durable convergence query: no executable, path, arguments, environment,
/// working directory, or status payload is caller-selectable.
///
/// The implementation is Linux/POSIX-specific. It atomically materializes the
/// independently verified script, persists the private controller and runtime
/// process identities before releasing a pre-exec gate. If Manager death
/// prevents that release, the controller and gated child retain the inherited
/// cross-process slot-lock description until the unreleased pair exits. It
/// observes before start, reconnects to the surviving controller after Manager
/// replacement, and produces a progress result only from a newly received
/// complete source status record. Only the exact fixed shutdown protocol can
/// stop the process.
class RetainedPosixRuntimeUnitAdapter final : public IRuntimeUnitProcessPort,
                                              public IRuntimeUnitProgressPort {
public:
  RetainedPosixRuntimeUnitAdapter(
      std::filesystem::path trusted_state_root,
      std::filesystem::path trusted_run_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system, std::string shell_contract);
  ~RetainedPosixRuntimeUnitAdapter() override;

  RetainedPosixRuntimeUnitAdapter(const RetainedPosixRuntimeUnitAdapter &) =
      delete;
  RetainedPosixRuntimeUnitAdapter &
  operator=(const RetainedPosixRuntimeUnitAdapter &) = delete;
  RetainedPosixRuntimeUnitAdapter(RetainedPosixRuntimeUnitAdapter &&) = delete;
  RetainedPosixRuntimeUnitAdapter &
  operator=(RetainedPosixRuntimeUnitAdapter &&) = delete;

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const manyforge::core::RuntimeEpoch &runtime_epoch) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::component_manager
