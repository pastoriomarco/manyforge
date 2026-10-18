#include <algorithm>
#include <deque>
#include <manyforge_core/io_signal_ingress.hpp>
#include <mutex>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

bool valid_reason(IoIngressLossReason reason) {
  switch (reason) {
    case IoIngressLossReason::kMiddlewareMessageLost:
    case IoIngressLossReason::kHostIngressRejected:
    case IoIngressLossReason::kSequenceExhausted:
    case IoIngressLossReason::kDisconnected:
      return true;
  }
  return false;
}

}  // namespace

struct IoSignalIngress::State final {
  struct Barrier final {
    // IDs are allocated at installation, never while recording a full-queue loss.
    IoIngressLossBarrierDTO evidence;
    bool active{};
  };
  struct Source final {
    IoSignalIngressSourceDTO binding;
    std::deque<IoSourceAcquisitionDTO> acquisitions;
    Barrier pending;
    Barrier in_flight;
  };

  State(std::shared_ptr<IoSignalSubsystem> supplied_signals,
        const IoSignalIngressConfigurationDTO& configuration)
      : signals(std::move(supplied_signals)) {
    for (const auto& source : configuration.sources) {
      IoIngressLossBarrierDTO barrier{source.source_id, source.incarnation, 0, 0, 0,
                                       false, false, false, false, false, std::nullopt};
      sources.push_back({source, {}, {barrier, false}, {barrier, false}});
    }
  }

  Source* Find(const InterruptSourceId& source, const IoSourceIncarnation& incarnation) {
    const auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& entry) {
      return entry.binding.source_id == source && entry.binding.incarnation == incarnation;
    });
    return found == sources.end() ? nullptr : &*found;
  }

  void Record(Source& source, TimePoint received, std::uint64_t after, IoIngressLossReason reason,
              std::optional<std::uint64_t> count = std::nullopt) {
    auto& slot = source.pending;
    auto& evidence = slot.evidence;
    if (!slot.active) {
      evidence.first_received_at_ns = received;
      evidence.last_received_at_ns = received;
      evidence.after_ingress_source_sequence = after;
      evidence.middleware_message_lost = false;
      evidence.host_ingress_rejected = false;
      evidence.sequence_exhausted = false;
      evidence.disconnected = false;
      evidence.coalesced = false;
      evidence.middleware_reported_lost_messages = count;
      slot.active = true;
    } else {
      evidence.first_received_at_ns = std::min(evidence.first_received_at_ns, received);
      evidence.last_received_at_ns = std::max(evidence.last_received_at_ns, received);
      evidence.after_ingress_source_sequence =
          std::max(evidence.after_ingress_source_sequence, after);
      evidence.coalesced = true;
      // No inferred count, including when two actual DDS notifications coalesce.
      evidence.middleware_reported_lost_messages.reset();
    }
    switch (reason) {
      case IoIngressLossReason::kMiddlewareMessageLost:
        evidence.middleware_message_lost = true;
        break;
      case IoIngressLossReason::kHostIngressRejected:
        evidence.host_ingress_rejected = true;
        break;
      case IoIngressLossReason::kSequenceExhausted:
        evidence.sequence_exhausted = true;
        break;
      case IoIngressLossReason::kDisconnected:
        evidence.disconnected = true;
        break;
    }
  }

  std::shared_ptr<IoSignalSubsystem> signals;
  std::vector<Source> sources;
  std::size_t next_source{};
  std::mutex queue_mutex;
  std::mutex drain_mutex;
};

Result<std::unique_ptr<IoSignalIngress>> IoSignalIngress::Create(
    std::shared_ptr<IoSignalSubsystem> signals) {
  using Reply = Result<std::unique_ptr<IoSignalIngress>>;
  if (!signals)
    return Reply::Err({"invalid_request", "I/O ingress requires its signal subsystem", {}});
  const auto configuration = signals->IngressConfiguration();
  if (!configuration.ok()) return Reply::Err(configuration.error());
  return Reply::Ok(std::unique_ptr<IoSignalIngress>(
      new IoSignalIngress(std::make_unique<State>(std::move(signals), configuration.value()))));
}

IoSignalIngress::IoSignalIngress(std::unique_ptr<State> state) : state_(std::move(state)) {}
IoSignalIngress::~IoSignalIngress() = default;

bool IoSignalIngress::TryEnqueue(const IoSourceAcquisitionDTO& acquisition) noexcept {
  try {
    // Core admission is authoritative and non-mutating; drain rechecks it after
    // any host installation/change that races this first check.
    const auto valid = state_->signals->ValidateAcquisition(acquisition);
    if (!valid.ok()) return false;
    std::lock_guard lock(state_->queue_mutex);
    auto* source = state_->Find(acquisition.source_id, acquisition.incarnation);
    if (!source || acquisition.source_sequence <= source->binding.last_source_sequence ||
        (source->binding.last_received_at_ns &&
         acquisition.trusted_received_at_ns < *source->binding.last_received_at_ns))
      return false;
    source->binding.last_source_sequence = acquisition.source_sequence;
    source->binding.last_received_at_ns = acquisition.trusted_received_at_ns;
    if (source->pending.active || source->in_flight.active ||
        source->acquisitions.size() == source->binding.records_per_source) {
      state_->Record(*source, acquisition.trusted_received_at_ns, acquisition.source_sequence,
                     IoIngressLossReason::kHostIngressRejected);
      return false;
    }
    try {
      source->acquisitions.push_back(acquisition);
    } catch (...) {
      state_->Record(*source, acquisition.trusted_received_at_ns, acquisition.source_sequence,
                     IoIngressLossReason::kHostIngressRejected);
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

void IoSignalIngress::NotifyLoss(const IoIngressLossNotificationDTO& loss) noexcept {
  if (loss.trusted_received_at_ns < 0 || !valid_reason(loss.reason) ||
      (loss.middleware_reported_lost_messages &&
       loss.reason != IoIngressLossReason::kMiddlewareMessageLost))
    return;
  try {
    std::lock_guard lock(state_->queue_mutex);
    auto* source = state_->Find(loss.source_id, loss.incarnation);
    if (!source) return;
    // Actual loss is not CAS-discarded merely because a sample was accepted
    // since the event was observed. The retained barrier orders before recovery.
    state_->Record(*source, loss.trusted_received_at_ns, loss.after_ingress_source_sequence,
                   loss.reason, loss.middleware_reported_lost_messages);
  } catch (...) {
    // No allocation occurs in Record: both bounded barrier slots are installed.
  }
}

Result<IoIngressDrainResultDTO> IoSignalIngress::Drain(std::size_t maximum_events) {
  using Reply = Result<IoIngressDrainResultDTO>;
  if (maximum_events == 0)
    return Reply::Err({"invalid_request", "I/O drain work bound must be positive", {}});
  std::lock_guard drain_lock(state_->drain_mutex);
  IoIngressDrainResultDTO result;
  while (result.attempted < maximum_events) {
    std::optional<IoSourceAcquisitionDTO> acquisition;
    std::optional<IoIngressLossBarrierDTO> barrier;
    std::size_t selected{};
    {
      std::lock_guard lock(state_->queue_mutex);
      bool found = false;
      for (std::size_t offset = 0; offset < state_->sources.size(); ++offset) {
        const auto index = (state_->next_source + offset) % state_->sources.size();
        auto& source = state_->sources[index];
        if (source.acquisitions.empty() && !source.pending.active && !source.in_flight.active)
          continue;
        selected = index;
        state_->next_source = (index + 1U) % state_->sources.size();
        if (!source.acquisitions.empty()) {
          acquisition = source.acquisitions.front();
        } else {
          if (!source.in_flight.active) {
            std::swap(source.in_flight, source.pending);
            source.pending.active = false;
          }
          barrier = source.in_flight.evidence;
        }
        found = true;
        break;
      }
      if (!found) break;
    }
    // Never hold the ingress lock across Core publication/watch evaluation.
    const auto applied = acquisition ? [&] {
      const auto published = state_->signals->Publish(*acquisition);
      return published.ok() ? Result<void>::Ok() : Result<void>::Err(published.error());
    }()
                                     : state_->signals->ReportIngressLoss(*barrier);
    ++result.attempted;
    // A replacement source cannot inherit old queued data or loss. Other errors
    // keep the exact applicable item, including an in-flight loss, for diagnosis.
    const bool obsolete = !applied.ok() && applied.error().code == "stale_generation";
    if (!applied.ok() && !obsolete) {
      result.blocked_error = applied.error();
      break;
    }
    {
      std::lock_guard lock(state_->queue_mutex);
      auto& source = state_->sources[selected];
      if (acquisition)
        source.acquisitions.pop_front();
      else
        source.in_flight.active = false;
    }
    if (obsolete)
      ++result.obsolete;
    else
      ++result.applied;
  }
  return Reply::Ok(std::move(result));
}

}  // namespace manyforge::core
