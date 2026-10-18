#pragma once

#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/ports/iio_signal_ingress.hpp>
#include <memory>

namespace manyforge::core {

struct IoIngressDrainResultDTO final {
  std::size_t attempted{};
  std::size_t applied{};
  std::size_t obsolete{};
  std::optional<ErrorDTO> blocked_error;
};

/// The existing host owns one ingress for a fixed installed catalog/source
/// incarnation set. Replace it when replacing those bindings; old queued data
/// cannot mutate new source incarnations. No thread, timer or scheduling owner.
/// Source queues reuse records_per_source. Drain is caller-driven round robin;
/// no liveness or hard nonblocking guarantee is made.
class IoSignalIngress final : public IIoSignalIngress {
 public:
  static Result<std::unique_ptr<IoSignalIngress>> Create(
      std::shared_ptr<IoSignalSubsystem> signals);
  ~IoSignalIngress() override;
  IoSignalIngress(const IoSignalIngress&) = delete;
  IoSignalIngress& operator=(const IoSignalIngress&) = delete;

  bool TryEnqueue(const IoSourceAcquisitionDTO& acquisition) noexcept override;
  void NotifyLoss(const IoIngressLossNotificationDTO& loss) noexcept override;
  /// At most maximum_events Core admissions, with fair source rotation. An
  /// applicable failed loss admission remains retained and blocks recovery;
  /// stale incarnation records retire harmlessly. Errors remain visible here.
  Result<IoIngressDrainResultDTO> Drain(std::size_t maximum_events);

 private:
  struct State;
  explicit IoSignalIngress(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

}  // namespace manyforge::core
