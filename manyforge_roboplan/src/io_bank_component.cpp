// Copyright 2026 Marco Pastorio
// SPDX-License-Identifier: Apache-2.0

#include <manyforge_roboplan/io_bank_component.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend {

using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentLifecycle;

namespace {

constexpr std::string_view kStreamId = "state";

[[nodiscard]] SetOutputResult write_failed(std::string code, std::string message,
                                           const ActionOutcome outcome = ActionOutcome::kFailed,
                                           std::optional<std::string> vendor = std::nullopt) {
  return {outcome, core::ActionError{std::move(code), std::move(message), std::move(vendor),
                                     std::nullopt}};
}

[[nodiscard]] SetOutputResult unknown(const std::string &why) {
  return write_failed("io_output_acceptance_unknown",
                      "the output write's outcome is unknown (" + why + "); it isn't resent",
                      ActionOutcome::kUnknown);
}

[[nodiscard]] core::ActionResult action_result(const ComponentAction action,
                                               const ActionOutcome outcome,
                                               std::optional<core::ActionError> error = {}) {
  core::ActionResult made;
  made.action = action;
  made.outcome = outcome;
  made.error = std::move(error);
  return made;
}

} // namespace

std::int64_t io_bank_stale_after_ns(const double rate_hz) {
  if (!(rate_hz > 0.0)) throw std::invalid_argument("an I/O bank's rate_hz must be positive");
  return kIoBankStalePeriods * static_cast<std::int64_t>(std::llround(1e9 / rate_hz));
}

IoBankComponentConfig io_bank_config_from_row(const std::string &component_id,
                                              const content_identity::CanonicalValue &lane_row) {
  const auto fail = [&](const std::string &what) {
    throw std::invalid_argument("I/O bank " + component_id + ": " + what);
  };
  if (!lane_row.is_object()) fail("its lane row is not an object");
  for (const auto &[key, unused] : lane_row.as_object()) {
    static_cast<void>(unused);
    if (key != "adapter" && key != "rate_hz" && key != "inputs" && key != "outputs" &&
        key != "indexes")
      fail("its lane row has the unknown key " + key);
  }
  const auto *adapter = lane_row.find_object_member("adapter");
  if (adapter == nullptr || !adapter->is_string() ||
      adapter->as_string() != kIoBankAdapterFamily)
    fail("its lane row needs adapter ros_io_bank");
  const auto *rate = lane_row.find_object_member("rate_hz");
  if (rate == nullptr || !rate->is_number() || !(rate->as_number() > 0.0) ||
      rate->as_number() > 1000.0)
    fail("its lane row needs rate_hz, more than 0 and at most 1000");
  const auto *indexes = lane_row.find_object_member("indexes");
  if (indexes != nullptr && !indexes->is_object()) fail("its signal indexes are not an object");
  IoBankComponentConfig config;
  config.component_id = component_id;
  config.rate_hz = rate->as_number();
  for (const auto direction : {core::IoSignalDirection::kInput, core::IoSignalDirection::kOutput}) {
    const bool output = direction == core::IoSignalDirection::kOutput;
    const auto *names = lane_row.find_object_member(output ? "outputs" : "inputs");
    if (names == nullptr) continue;
    if (!names->is_array()) fail("its signal names are not a list");
    for (const auto &name : names->as_array()) {
      if (!name.is_string()) fail("a signal name is not text");
      const auto *index =
          indexes == nullptr ? nullptr : indexes->find_object_member(name.as_string());
      if (index == nullptr || !index->is_number())
        fail("signal " + name.as_string() + " has no index (ros_io_bank needs one per signal)");
      const double number = index->as_number();
      if (number < 0.0 || number > 63.0 || number != std::floor(number))
        fail("signal " + name.as_string() + " has index outside 0..63");
      config.signals.push_back({name.as_string(), direction, static_cast<int>(number)});
    }
  }
  if (config.signals.empty()) fail("it declares no signals");
  return config;
}

IoBankComponent::IoBankComponent(IoBankComponentConfig config, core::IClock &clock)
    : config_(std::move(config)), clock_(clock),
      tracker_(config_.component_id, config_.signals, io_bank_stale_after_ns(config_.rate_hz)) {
  for (std::size_t index = 0U; index < config_.signals.size(); ++index)
    for (std::size_t other = 0U; other < index; ++other)
      if (config_.signals[index].direction == config_.signals[other].direction &&
          config_.signals[index].index == config_.signals[other].index)
        throw std::invalid_argument("an I/O bank's index is used twice in one direction: " +
                                    config_.signals[index].name);
}

void IoBankComponent::Attach(std::shared_ptr<IIoBankLink> link) {
  std::lock_guard lock(mutex_);
  link_ = std::move(link);
}

std::uint32_t IoBankComponent::NextSeqLocked() {
  auto seq = *next_seq_;
  // Never the device's last applied number: it would read as already applied.
  if (seq == last_applied_seq_) ++seq;
  next_seq_ = seq + 1U;
  return seq;
}

void IoBankComponent::NotifyLocked() {
  ++changes_;
  changed_.notify_all();
}

void IoBankComponent::EndLocked(Command &command, const std::optional<std::int64_t> decided_at_ns) {
  command.ended = true;
  command.decided_at_ns = decided_at_ns;
  if (in_flight_.get() == &command) in_flight_.reset();
  NotifyLocked();
}

void IoBankComponent::UnknownLocked(const std::shared_ptr<Command> &command,
                                    const std::string &why) {
  for (const auto &write : command->writes) write->result = unknown(why);
  command->outcome = ActionOutcome::kUnknown;
}

std::vector<core::IoSignalUpdateDTO> IoBankComponent::OnState(const IoBankStateSample &sample) {
  std::lock_guard lock(mutex_);
  last_any_sample_at_ns_ = sample.received_at_ns;
  // The same cycle again: the device's program didn't refresh the state.
  if (last_cycle_ && sample.cycle == *last_cycle_) return {};
  // A cycle that goes back: the device restarted (a wrap of the 32-bit
  // counter reads the same way, harmlessly).
  const bool new_epoch = last_cycle_ && sample.cycle < *last_cycle_;
  const bool first = !last_cycle_;
  last_cycle_ = sample.cycle;
  last_applied_seq_ = sample.applied_seq;
  ++position_;
  if (first || new_epoch) next_seq_ = sample.applied_seq + 1U;
  const auto refused = [&](const Write &write) {
    return ((sample.refused >> tracker_.signals()[write.signal].index) & 1U) != 0U;
  };
  const bool decided = sample.applied_result == kIoBankResultAccepted ||
                       sample.applied_result == kIoBankResultRefused;
  // What this sample decides: before the sample is applied (an accepted or
  // refused write's own sample is authoritative) or after it (an unanswered
  // command's outputs stay unknown until the next sample).
  std::shared_ptr<Command> unanswered;
  if (new_epoch) {
    // Every value is reported afresh from the device; the command in
    // flight's outcome is unknown.
    tracker_.NewEpoch();
    late_.reset();
    if (const auto command = in_flight_) {
      UnknownLocked(command, "the device restarted");
      EndLocked(*command, std::nullopt);
    }
  } else if (const auto command = in_flight_) {
    if (sample.applied_seq == command->seq && decided) {
      bool any_refused = false;
      for (const auto &write : command->writes) {
        if (refused(*write)) {
          any_refused = true;
          tracker_.Release(write->signal);
          write->result = write_failed("io_output_source_rejected",
                                       "the device refused the write", ActionOutcome::kFailed,
                                       "device_code=" + std::to_string(sample.device_code));
        } else {
          tracker_.WriteAccepted(write->signal, write->value, position_ - 1);
          write->result = SetOutputResult{ActionOutcome::kSucceeded, std::nullopt};
        }
      }
      command->outcome = any_refused ? ActionOutcome::kFailed : ActionOutcome::kSucceeded;
      EndLocked(*command, sample.received_at_ns);
    } else if (sample.applied_seq == command->seq) {
      unanswered = command;
      UnknownLocked(command, "the device reported result " +
                                 std::to_string(sample.applied_result));
      EndLocked(*command, sample.received_at_ns);
    } else if (++command->samples_seen >= kIoBankUnansweredSamples) {
      unanswered = command;
      UnknownLocked(command, "not applied within " +
                                 std::to_string(kIoBankUnansweredSamples) + " device cycles");
      EndLocked(*command, std::nullopt);
    }
  } else if (late_ && sample.applied_seq == late_->seq) {
    // A command reported unknown, applied late: its effect, not an outside change.
    if (decided)
      for (const auto &write : late_->writes)
        if (!refused(*write)) tracker_.WriteAccepted(write->signal, write->value, position_ - 1);
    late_.reset();
  }
  auto updates =
      tracker_.OnSample(sample.inputs, sample.outputs, sample.received_at_ns, position_);
  if (unanswered) {
    for (const auto &write : unanswered->writes)
      tracker_.WriteUnanswered(write->signal, write->value);
    late_ = unanswered;
  }
  return updates;
}

std::vector<core::SignalSample> IoBankComponent::Samples(const std::int64_t now_ns) const {
  std::lock_guard lock(mutex_);
  return tracker_.Samples(now_ns);
}

core::ComponentState IoBankComponent::State(const std::int64_t now_ns) const {
  core::ComponentState state;
  state.observed_at_ns = now_ns;
  std::lock_guard lock(mutex_);
  const bool fresh = tracker_.Fresh(now_ns);
  core::ComponentStream stream{std::string(kStreamId), core::StreamFreshness::kNone, std::nullopt};
  if (const auto &last = tracker_.LastSampleAt()) {
    stream.freshness = fresh ? core::StreamFreshness::kFresh : core::StreamFreshness::kStale;
    stream.age_ns = now_ns - *last;
  }
  state.streams.push_back(stream);
  state.lifecycle = lifecycle_;
  state.activity = in_flight_ || !waiting_.empty() ? core::ComponentActivity::kBusy
                   : tracker_.AnyUnknown()          ? core::ComponentActivity::kUnknown
                                                    : core::ComponentActivity::kIdle;
  if (lifecycle_ == ComponentLifecycle::kNotPrepared || fresh) {
    stale_since_.reset();
    return state;
  }
  if (!stale_since_) stale_since_ = now_ns;
  // Samples still arriving with a frozen cycle: the device's program stopped.
  const bool repeats = last_any_sample_at_ns_ &&
                       now_ns - *last_any_sample_at_ns_ <= tracker_.StaleAfterNs();
  state.lifecycle = ComponentLifecycle::kFault;
  state.fault = core::Fault{core::FaultCode::kStreamStale,
                            "the I/O bank's state stream stopped",
                            true,
                            core::FaultOperatorAction::kRecover,
                            repeats ? std::optional<std::string>("cycle_stopped") : std::nullopt,
                            std::nullopt,
                            {},
                            *stale_since_};
  return state;
}

std::vector<core::Capability> IoBankComponent::capabilities() const {
  return {core::Capability::kIoManualWrite, core::Capability::kIoOutputReadback};
}

core::ActionResult IoBankComponent::Run(const ComponentAction action, const std::stop_token stop) {
  if (action == ComponentAction::kPrepare || action == ComponentAction::kRecover ||
      action == ComponentAction::kEnable) {
    // A fresh sample within the lifecycle command time.
    const auto deadline = clock_.now() + config_.action_timeout.count();
    const auto fresh = [&] {
      std::lock_guard lock(mutex_);
      return tracker_.Fresh(clock_.now());
    };
    while (!fresh() && !stop.stop_requested() && clock_.now() < deadline) {
      if (config_.pause)
        config_.pause();
      else
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  std::lock_guard lock(mutex_);
  switch (action) {
  case ComponentAction::kPrepare:
  case ComponentAction::kRecover:
  case ComponentAction::kEnable:
    if (!tracker_.Fresh(clock_.now()))
      return action_result(action, ActionOutcome::kFailed,
                           core::ActionError{"fault_stream_stale",
                                             "the I/O bank's state stream isn't arriving",
                                             std::nullopt, std::nullopt});
    lifecycle_ = action == ComponentAction::kEnable ? ComponentLifecycle::kEnabled
                                                    : ComponentLifecycle::kPrepared;
    return action_result(action, ActionOutcome::kSucceeded);
  case ComponentAction::kDisable:
    if (lifecycle_ == ComponentLifecycle::kEnabled) lifecycle_ = ComponentLifecycle::kPrepared;
    return action_result(action, ActionOutcome::kSucceeded);
  case ComponentAction::kResetPosition:
  case ComponentAction::kPowerOff:
  case ComponentAction::kReboot:
  case ComponentAction::kSimReset: break;
  }
  return action_result(action, ActionOutcome::kFailed,
                       core::ActionError{"component_action_unsupported",
                                         "an I/O bank doesn't support this action", std::nullopt,
                                         std::nullopt});
}

std::size_t IoBankComponent::WaitingWrites() const {
  std::lock_guard lock(mutex_);
  return waiting_.size();
}

SetOutputResult IoBankComponent::SetOutput(const std::string_view name,
                                           const std::variant<bool, double> requested,
                                           const std::stop_token stop) {
  const auto found = tracker_.Find(name);
  if (!found || tracker_.signals()[*found].direction != core::IoSignalDirection::kOutput)
    return write_failed("io_signal_unknown", "'" + std::string(name) +
                                                 "' is not an output this I/O bank's model "
                                                 "declares");
  if (!std::holds_alternative<bool>(requested))
    return write_failed("io_signal_type_mismatch",
                        std::string(name) + " is a digital output; its value is true or false");
  const auto write = std::make_shared<Write>(Write{*found, std::get<bool>(requested), std::nullopt});
  const auto not_sendable = [&]() -> std::optional<SetOutputResult> {
    if (lifecycle_ != ComponentLifecycle::kEnabled)
      return write_failed("io_output_not_submitted",
                          "the I/O component is not enabled; nothing was sent");
    if (!link_ || !next_seq_ || !tracker_.Fresh(clock_.now()))
      return write_failed("io_output_not_submitted",
                          "the I/O bank's state stream isn't arriving; nothing was sent");
    return std::nullopt;
  };
  const auto stopped = [] {
    return write_failed("component_action_interrupted_by_stop",
                        "a Stop came before the output write was sent; nothing was sent");
  };
  std::unique_lock lock(mutex_);
  if (auto refusal = not_sendable()) return *refusal;
  if (stop.stop_requested()) return stopped();
  waiting_.push_back(write);
  // The command this caller sent, for its debug record.
  std::shared_ptr<Command> sent;
  std::int64_t sent_at_ns = 0;
  while (!write->result) {
    const auto queued = std::find(waiting_.begin(), waiting_.end(), write);
    if (queued != waiting_.end()) {
      // Not sent yet: a Stop removes only this write.
      if (stop.stop_requested()) {
        waiting_.erase(queued);
        NotifyLocked();
        return stopped();
      }
      if (!in_flight_ && queued == waiting_.begin()) {
        // The oldest waiting write's caller sends the next command.
        if (auto refusal = not_sendable()) {
          waiting_.erase(queued);
          NotifyLocked();
          return *refusal;
        }
        auto command = std::make_shared<Command>();
        command->seq = NextSeqLocked();
        IoBankCommandMessage message;
        message.seq = command->seq;
        message.values.fill(kIoBankOutputKeep);
        // Merge the waiting writes in arrival order, at most one per output.
        std::uint64_t taken = 0U;
        for (auto waiting = waiting_.begin(); waiting != waiting_.end();) {
          const auto bit = tracker_.signals()[(*waiting)->signal].index;
          if (((taken >> bit) & 1U) != 0U) {
            ++waiting;
            continue;
          }
          taken |= std::uint64_t{1} << bit;
          message.values[static_cast<std::size_t>(bit)] =
              (*waiting)->value ? kIoBankOutputOn : kIoBankOutputOff;
          tracker_.Hold((*waiting)->signal);
          command->writes.push_back(*waiting);
          waiting = waiting_.erase(waiting);
        }
        in_flight_ = command;
        late_.reset();
        const auto link = link_;
        lock.unlock();
        sent_at_ns = clock_.now();
        const bool published = link->Send(message);
        lock.lock();
        if (!published && !command->ended) {
          for (const auto &carried : command->writes) {
            tracker_.Release(carried->signal);
            carried->result =
                write_failed("io_output_not_submitted",
                             "the I/O bank's command couldn't be published; nothing was sent");
          }
          EndLocked(*command, std::nullopt);
        } else if (published) {
          sent = command;
        }
        continue;
      }
    }
    const auto seen = changes_;
    if (const auto command = in_flight_) {
      // Sent: a Stop no longer ends it (OD-213). It ends on a sample (its
      // decision, K samples, a device restart) or when the stream goes stale.
      const auto remaining = *tracker_.LastSampleAt() + tracker_.StaleAfterNs() - clock_.now();
      if (remaining < 0) {
        for (const auto &carried : command->writes)
          tracker_.WriteUnanswered(carried->signal, carried->value);
        UnknownLocked(command, "the I/O bank's state stream went stale");
        late_ = command;
        EndLocked(*command, std::nullopt);
        continue;
      }
      changed_.wait_for(lock, queued != waiting_.end() ? stop : std::stop_token{},
                        std::chrono::nanoseconds(remaining + 1),
                        [&] { return changes_ != seen; });
    } else {
      changed_.wait(lock, queued != waiting_.end() ? stop : std::stop_token{},
                    [&] { return changes_ != seen; });
    }
  }
  const auto result = *write->result;
  std::optional<IoBankWriteRecord> record;
  if (sent)
    record = IoBankWriteRecord{sent->seq, sent->writes.size(), sent_at_ns, sent->decided_at_ns,
                               sent->outcome};
  const auto link = link_;
  lock.unlock();
  if (record && link) link->Record(*record);
  return result;
}

} // namespace manyforge::roboplan_backend
