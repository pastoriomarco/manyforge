#include <algorithm>
#include <limits>
#include <manyforge_ros2_control_execution/volatile_motion_trace.hpp>

namespace manyforge::ros2_control_execution {
namespace {
using V = content_identity::CanonicalValue;
V number(double value) {
  // JSON has one zero; preserve the captured IEEE value until export.
  return V(value == 0.0 ? 0.0 : value);
}
V ns(core::TimePoint n) {
  return n >= 0 ? content_identity::nonnegative_int64_value(n)
                : V(std::to_string(n));
}
V count(std::uint64_t n) {
  // Strong identities span uint64: preserve safe-number bytes and use exact
  // canonical decimal text above the I-JSON integer boundary, never saturate.
  return n <= 9007199254740991ULL ? V(static_cast<double>(n))
                                  : V(std::to_string(n));
}
V numbers(const std::vector<double> &input) {
  V::Array out;
  out.reserve(input.size());
  for (double value : input)
    out.emplace_back(number(value));
  return V(std::move(out));
}
V optional_time(const std::optional<core::TimePoint> &value) {
  return value ? ns(*value) : V(nullptr);
}
V key_value(const VolatileMotionTrace::Token &t) {
  return V(
      V::Object{{"action", V(t.action)},
                {"binding_epoch", count(t.epoch.value())},
                {"controller_goal_id", V(t.key.controller_goal_id.value())},
                {"controller_goal_generation",
                 count(t.key.controller_goal_generation.value())}});
}
V replacement_policy(bool replacement,
                     const std::optional<core::ExecutionReplacementStartDTO> &start) {
  if (!replacement) return V(nullptr);
  V experiment(nullptr);
  if (start && start->experiment) {
    const auto &e = *start->experiment;
    experiment = V(V::Object{
        {"producer_policy", V(nullptr)},
        {"release_policy", V(e.release_local_ns ? "timer" : "immediate")},
        {"release_hold", V(e.release_hold == core::ExecutionReleaseHold::kHeld ? "held" :
                           e.release_hold == core::ExecutionReleaseHold::kMissed ? "missed" :
                           e.release_hold == core::ExecutionReleaseHold::kDeadline ? "deadline" : "none")},
        {"stamp_policy", V(e.midtick_stamp ? "midtick" : "grid")},
        {"payload_policy", V(e.spring_back_fast ? "spring_back_fast_v0" :
                             e.v0_payload ? "v0" : "v6_prefix")},
        {"start_allowance_override", V(e.start_allowance_override)},
        {"start_allowance_ns", ns(e.start_allowance_ns)},
        {"factor", number(e.factor)}});
  }
  return V(V::Object{{"schedule", V(start ? "controller_stamp" : "immediate")},
                    {"controller_time_reference", V(nullptr)},
                    {"experiment", std::move(experiment)}});
}
std::size_t payload_bytes(const VolatileMotionTrace::Goal &g) {
  std::size_t bytes =
      sizeof(g) + g.trajectory.header.frame_id.capacity() +
      g.trajectory.joint_names.capacity() * sizeof(std::string) +
      g.trajectory.points.capacity() *
          sizeof(trajectory_msgs::msg::JointTrajectoryPoint) +
      g.goal_tolerance.capacity() * sizeof(control_msgs::msg::JointTolerance) +
      g.path_tolerance.capacity() * sizeof(control_msgs::msg::JointTolerance);
  for (const auto &name : g.trajectory.joint_names)
    bytes += name.capacity();
  for (const auto &tolerance : g.goal_tolerance)
    bytes += tolerance.name.capacity();
  for (const auto &tolerance : g.path_tolerance)
    bytes += tolerance.name.capacity();
  for (const auto &p : g.trajectory.points)
    bytes +=
        sizeof(double) * (p.positions.capacity() + p.velocities.capacity() +
                          p.accelerations.capacity() + p.effort.capacity());
  return bytes;
}
V goal_value(const VolatileMotionTrace::Goal &g) {
  V::Array names, points;
  for (const auto &name : g.trajectory.joint_names)
    names.emplace_back(name);
  points.reserve(g.trajectory.points.size());
  for (const auto &p : g.trajectory.points)
    points.emplace_back(V::Object{
        {"time_sec", V(static_cast<double>(p.time_from_start.sec))},
        {"time_nanosec", V(static_cast<double>(p.time_from_start.nanosec))},
        {"p", numbers(p.positions)},
        {"v", numbers(p.velocities)},
        {"a", numbers(p.accelerations)},
        {"effort", numbers(p.effort)}});
  return V(V::Object{
      {"header",
       V(V::Object{
           {"sec", V(static_cast<double>(g.trajectory.header.stamp.sec))},
           {"nanosec",
            V(static_cast<double>(g.trajectory.header.stamp.nanosec))},
           {"frame_id", V(g.trajectory.header.frame_id)}})},
      {"joint_names", V(std::move(names))},
      {"points", V(std::move(points))}});
}
} // namespace
bool VolatileMotionTrace::Matches(const Token &a, const Token &b) noexcept {
  return a.action == b.action && a.epoch == b.epoch && a.key == b.key;
}
std::shared_ptr<void> VolatileMotionTrace::Charge(std::size_t bytes) {
  constexpr std::size_t limit = 2U * kGoalBytes;
  auto current = allocated_->load();
  do {
    if (bytes > limit || current > limit - bytes)
      return {};
  } while (!allocated_->compare_exchange_weak(current, current + bytes));
  return std::shared_ptr<void>(
      allocated_.get(),
      [counter = allocated_, bytes](void *) { counter->fetch_sub(bytes); });
}
void VolatileMotionTrace::Preregister(
    const std::string &action, core::ExecutionBindingEpoch epoch,
    const core::ExecutionGoalKey &key,
    const core::PreparedOperationParticipantDTO &p) noexcept {
  try {
    const auto bounded = [](const std::string &s) { return s.size() <= 2048U; };
    if (!bounded(action) || !bounded(key.controller_goal_id.value()) ||
        !bounded(p.operation_id.value()) ||
        !bounded(p.participant_id.value()) ||
        !bounded(p.program_run_id.value()) ||
        !bounded(p.program_binding_id.value()) ||
        !bounded(p.controllable_component_id.value())) {
      ++failures_;
      return;
    }
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      ++failures_;
      return;
    }
    Token token{action, epoch, key, {}};
    Context context{p.operation_id.value(),
                    p.participant_id.value(),
                    p.program_run_id.value(),
                    p.program_binding_id.value(),
                    p.controllable_component_id.value(),
                    p.operation_revision.value()};
    for (auto &pending : pending_)
      if (Matches(pending.token, token)) {
        pending.context = std::move(context);
        return;
      }
    if (pending_.size() == kGoalLimit) {
      pending_.pop_front();
      ++omitted_;
    }
    pending_.push_back({std::move(token), std::move(context)});
  } catch (...) {
    ++failures_;
  }
}
void VolatileMotionTrace::Evict(Garbage &garbage) {
  auto &entry = goals_.front();
  if (entry.payload)
    garbage.push_back(std::move(entry.payload));
  goal_bytes_ -= entry.bytes;
  goals_.pop_front();
  ++evicted_;
}
VolatileMotionTrace::TokenPtr VolatileMotionTrace::Begin(
    const std::string &action, core::ExecutionBindingEpoch epoch,
    const core::ExecutionGoalKey &key, const TokenPtr &predecessor,
    core::TimePoint attempted_at, Garbage &garbage) noexcept {
  try {
    if (action.size() > 2048U ||
        key.controller_goal_id.value().size() > 2048U) {
      ++failures_;
      return {};
    }
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      ++failures_;
      return {};
    }
    Token token{action, epoch, key, {}};
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
      if (Matches(it->token, token)) {
        token.context = std::move(it->context);
        pending_.erase(it);
        break;
      }
    if (!token.context && predecessor && predecessor->epoch == epoch &&
        predecessor->action == action &&
        predecessor->key.controller_goal_id == key.controller_goal_id &&
        predecessor->key.controller_goal_generation.value() != UINT64_MAX &&
        predecessor->key.controller_goal_generation.value() + 1U ==
            key.controller_goal_generation.value())
      token.context = predecessor->context;
    token.allocation_charge = Charge(64U * 1024U);
    if (!token.allocation_charge) {
      ++omitted_;
      return {};
    }
    auto pointer = std::make_shared<const Token>(std::move(token));
    // Count- and string-bounded metadata reservation includes token/context,
    // observation identities and allocator overhead. Payload is charged by
    // capacity.
    constexpr std::size_t metadata_bytes = 64U * 1024U;
    while (!goals_.empty() && (goals_.size() >= kGoalLimit ||
                               metadata_bytes > kGoalBytes - goal_bytes_))
      Evict(garbage);
    Entry entry{};
    entry.token = pointer;
    entry.context = pointer->context;
    entry.predecessor = predecessor;
    entry.attempted_at = attempted_at;
    entry.bytes = metadata_bytes;
    entry.facts.key = key;
    entry.facts.binding_epoch = epoch;
    goals_.push_back(std::move(entry));
    goal_bytes_ += metadata_bytes;
    return pointer;
  } catch (...) {
    ++failures_;
    return {};
  }
}
void VolatileMotionTrace::Attach(const TokenPtr &token, Goal &&goal,
                                 core::TimePoint returned_at,
                                 core::ExecutionPortDispatch disposition,
                                 Garbage &garbage, bool replacement,
                                 const std::optional<core::ExecutionReplacementStartDTO> &start) noexcept {
  if (!token)
    return;
  try {
    const auto bytes = payload_bytes(goal);
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      ++failures_;
      return;
    }
    auto find = [&] {
      return std::find_if(goals_.begin(), goals_.end(),
                          [&](const Entry &e) { return e.token == token; });
    };
    auto it = find();
    if (it == goals_.end()) {
      ++omitted_;
      return;
    }
    it->returned_at = returned_at;
    it->disposition = disposition;
    it->replacement = replacement;
    it->replacement_start = start;
    if (bytes > kGoalBytes - it->bytes) {
      ++omitted_;
      return;
    }
    while (bytes > kGoalBytes - goal_bytes_ && goals_.front().token != token) {
      Evict(garbage);
      it = find();
    }
    if (bytes > kGoalBytes - goal_bytes_) {
      ++omitted_;
      return;
    }
    auto charge = Charge(bytes);
    if (!charge) {
      ++omitted_;
      return;
    }
    auto payload = std::shared_ptr<const Goal>(
        new Goal(std::move(goal)),
        [charge = std::move(charge)](const Goal *value) {
          (void)charge;
          delete value;
        });
    it->payload = std::move(payload);
    it->bytes += bytes;
    goal_bytes_ += bytes;
  } catch (...) {
    ++failures_;
  }
}
void VolatileMotionTrace::Observe(
    const TokenPtr &token,
    const core::ExecutionGoalObservationDTO &facts) noexcept {
  if (!token)
    return;
  try {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      ++failures_;
      return;
    }
    for (auto &e : goals_)
      if (e.token == token) {
        e.facts.mechanism_goal_id = facts.mechanism_goal_id;
        e.facts.phase = facts.phase;
        e.facts.result = facts.result;
        e.facts.accepted_stamp_ns = facts.accepted_stamp_ns;
        e.facts.terminal_local_receipt_stamp =
            facts.terminal_local_receipt_stamp;
        if (!e.first_own_feedback && facts.latest_feedback)
          e.first_own_feedback = facts.latest_feedback->local_receipt_stamp_ns;
        return;
      }
  } catch (...) {
    ++failures_;
  }
}
void VolatileMotionTrace::Sample(
    const std::string &action,
    const core::ExecutionControllerSampleDTO &sample) noexcept {
  try {
    if (action.size() > 2048U) {
      ++failures_;
      return;
    }
    std::size_t bytes = sizeof(SampleEntry) + action.size();
    for (const auto *v :
         {&sample.reference_positions_rad, &sample.reference_velocities_rad_s,
          &sample.reference_accelerations_rad_s2, &sample.command_positions_rad,
          &sample.measured_positions_rad, &sample.error_positions_rad})
      bytes += v->size() * sizeof(double);
    if (sample.command_velocities_rad_s)
      bytes += sample.command_velocities_rad_s->size() * sizeof(double);
    if (sample.measured_velocities_rad_s)
      bytes += sample.measured_velocities_rad_s->size() * sizeof(double);
    // Reserve allocator/copy capacity overhead conservatively for this bounded
    // copy.
    if (bytes > kSampleBytes / 2U) {
      ++failures_;
      return;
    }
    bytes *= 2U;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      ++failures_;
      return;
    }
    while (!samples_.empty() && (samples_.size() >= kSampleLimit ||
                                 bytes > kSampleBytes - sample_bytes_)) {
      sample_bytes_ -= samples_.front().bytes;
      samples_.pop_front();
      ++dropped_samples_;
    }
    samples_.push_back({action, sample, bytes});
    sample_bytes_ += bytes;
    if (sample.applied_factor) {
      auto found = std::find_if(scales_.rbegin(), scales_.rend(),
                                [&](const ScaleEntry &e) {
                                  return !e.published && e.action == action &&
                                         e.epoch == sample.binding_epoch;
                                });
      if (found == scales_.rend() || found->factor != *sample.applied_factor) {
        if (scales_.size() == 256U) {
          scales_.pop_front();
          ++evicted_scales_;
        }
        scales_.push_back({action, sample.binding_epoch,
                           sample.local_receipt_stamp_ns,
                           *sample.applied_factor, false});
      }
    }
  } catch (...) {
    ++failures_;
  }
}
void VolatileMotionTrace::PublishedScale(const std::string &action,
                                         core::ExecutionBindingEpoch epoch,
                                         core::TimePoint when,
                                         double factor) noexcept {
  try {
    if (action.size() > 2048U) {
      ++failures_;
      return;
    }
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      ++failures_;
      return;
    }
    if (scales_.size() == 256U) {
      scales_.pop_front();
      ++evicted_scales_;
    }
    scales_.push_back({action, epoch, when, factor, true});
  } catch (...) {
    ++failures_;
  }
}
core::Result<V>
VolatileMotionTrace::Snapshot(const core::RequestOwnerEpoch &owner,
                              std::size_t allowance) const {
  const auto error = [](const char *reason) {
    return core::Result<V>::Err(
        {"volatile_motion_trace_unavailable", reason, {}});
  };
  try {
    if (!allowance || allowance > kMaximumTraceBytes)
      return error("trace allowance is outside its bounded range");
    std::unique_lock snapshot_lock(snapshot_mutex_, std::try_to_lock);
    if (!snapshot_lock.owns_lock())
      return error("another bounded trace snapshot is in progress");
    std::deque<Entry> goals;
    std::deque<SampleEntry> samples;
    std::deque<ScaleEntry> scales;
    std::uint64_t evicted, omitted, dropped, evicted_scales;
    {
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock.owns_lock())
        return error("trace capture is busy");
      goals = goals_;
      samples = samples_;
      scales = scales_;
      evicted = evicted_;
      omitted = omitted_;
      dropped = dropped_samples_;
      evicted_scales = evicted_scales_;
    }
    V::Array goal_values, sample_values, scale_values, omission_values;
    bool truncated = false;
    std::uint64_t association_missing = 0;
    auto make = [&](std::size_t exported) {
      return V(V::Object{
          {"schema_version", V("manyforge.volatile_motion_trace.v1")},
          {"request_owner_epoch", V(owner.value())},
          {"volatile", V(true)},
          {"retention", V(V::Object{{"goal_limit", count(kGoalLimit)},
                                    {"goal_byte_limit", count(kGoalBytes)},
                                    {"sample_limit", count(kSampleLimit)},
                                    {"sample_byte_limit", count(kSampleBytes)},
                                    {"scale_event_limit", count(256U)}})},
          {"coverage",
           V(V::Object{{"truncated", V(truncated)},
                       {"evicted_goals", count(evicted)},
                       {"omitted_captures", count(omitted)},
                       {"omitted_goals", V(omission_values)},
                       {"dropped_samples", count(dropped)},
                       {"omitted_samples",
                        count(samples.size() - sample_values.size())},
                       {"evicted_scale_events", count(evicted_scales)},
                       {"omitted_scale_events",
                        count(scales.size() - scale_values.size())},
                       {"capture_failures", count(failures_.load())},
                       {"association_missing", count(association_missing)},
                       {"node_scope_unavailable", count(goal_values.size())},
                       {"exported_bytes", count(exported)}})},
          {"goals", V(goal_values)},
          {"controller_samples", V(sample_values)},
          {"scale_events", V(scale_values)}});
    };
    // Reserve bounded complete coverage for every omitted retained goal before
    // adding payloads, so an allowance cutoff never loses its own explanation.
    for (auto it = goals.rbegin(); it != goals.rend(); ++it)
      omission_values.emplace_back(V::Object{
          {"goal", key_value(*it->token)},
          {"reason",
           V(it->payload ? "response_allowance" : "payload_not_retained")}});
    truncated = !omission_values.empty();
    if (content_identity::canonical_json(make(allowance)).size() > allowance)
      return error("trace allowance cannot fit bounded omission coverage");
    std::size_t used =
        content_identity::canonical_json(make(allowance)).size() + 64U;
    for (auto it = goals.rbegin(); it != goals.rend(); ++it) {
      const auto &e = *it;
      if (!e.payload)
        continue;
      V::Object row{
          {"controller_goal_id", V(e.token->key.controller_goal_id.value())},
          {"controller_goal_generation",
           count(e.token->key.controller_goal_generation.value())},
          {"binding_epoch", count(e.token->epoch.value())},
          {"action", V(e.token->action)},
          {"predecessor",
           e.predecessor ? key_value(*e.predecessor) : V(nullptr)},
          {"mechanism_goal_id", e.facts.mechanism_goal_id.empty()
                                    ? V(nullptr)
                                    : V(e.facts.mechanism_goal_id.value())},
          {"send",
           V(V::Object{
               {"attempted_at_ns", ns(e.attempted_at)},
               {"returned_at_ns", optional_time(e.returned_at)},
               {"disposition",
                V(e.disposition == core::ExecutionPortDispatch::kDispatched
                      ? "dispatched"
                      : "effect_uncertain")}})},
          {"accepted_at_ns", optional_time(e.facts.accepted_stamp_ns)},
          {"first_own_feedback_at_ns", optional_time(e.first_own_feedback)},
          {"installation", V("unknown")},
          {"execution_scope", V(nullptr)},
          {"replacement_policy", replacement_policy(e.replacement, e.replacement_start)},
          {"terminal",
           e.facts.terminal_local_receipt_stamp
               ? V(V::Object{{"receipt_at_ns",
                              ns(*e.facts.terminal_local_receipt_stamp)},
                             {"result", e.facts.result ? V(core::to_string(
                                                             *e.facts.result))
                                                       : V(nullptr)}})
               : V(nullptr)},
          {"trajectory", goal_value(*e.payload)}};
      for (auto [name, value] :
           {std::pair{"operation_id",
                      e.context ? e.context->operation_id : std::string{}},
            {"participant_id",
             e.context ? e.context->participant_id : std::string{}},
            {"program_run_id",
             e.context ? e.context->program_run_id : std::string{}},
            {"program_binding_id",
             e.context ? e.context->program_binding_id : std::string{}},
            {"controllable_component_id",
             e.context ? e.context->component_id : std::string{}}})
        row.emplace_back(name, e.context ? V(std::move(value)) : V(nullptr));
      row.emplace_back("operation_revision",
                       e.context ? count(e.context->operation_revision)
                                 : V(nullptr));
      V row_value(std::move(row));
      const auto row_bytes =
          content_identity::canonical_json(row_value).size() + 1U;
      if (used > allowance || row_bytes > allowance - used) {
        truncated = true;
        continue;
      }
      used += row_bytes;
      goal_values.emplace_back(std::move(row_value));
      auto omitted_it = std::find_if(
          omission_values.begin(), omission_values.end(), [&](const V &v) {
            return *v.find_object_member("goal") == key_value(*e.token);
          });
      omission_values.erase(omitted_it);
      if (!e.context)
        ++association_missing;
      truncated = !omission_values.empty();
    }
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
      const auto &s = it->sample;
      V row(
          V::Object{{"action", V(it->action)},
                    {"binding_epoch", count(s.binding_epoch.value())},
                    {"source_stamp_ns", ns(s.source_stamp_ns)},
                    {"local_receipt_stamp_ns", ns(s.local_receipt_stamp_ns)},
                    {"cursor_s", number(s.cursor_s)},
                    {"reference_p", numbers(s.reference_positions_rad)},
                    {"reference_v", numbers(s.reference_velocities_rad_s)},
                    {"reference_a", numbers(s.reference_accelerations_rad_s2)},
                    {"commanded_p", numbers(s.command_positions_rad)},
                    {"commanded_v", s.command_velocities_rad_s
                                        ? numbers(*s.command_velocities_rad_s)
                                        : V(nullptr)},
                    {"measured_p", numbers(s.measured_positions_rad)},
                    {"measured_v", s.measured_velocities_rad_s
                                       ? numbers(*s.measured_velocities_rad_s)
                                       : V(nullptr)},
                    {"applied_factor",
                     s.applied_factor ? number(*s.applied_factor) : V(nullptr)}});
      const auto bytes = content_identity::canonical_json(row).size() + 1U;
      if (used > allowance || bytes > allowance - used) {
        truncated = true;
        break;
      }
      used += bytes;
      sample_values.emplace_back(std::move(row));
    }
    for (auto it = scales.rbegin(); it != scales.rend(); ++it) {
      V row(V::Object{
          {"action", V(it->action)},
          {"binding_epoch", count(it->epoch.value())},
          {"local_receipt_stamp_ns", ns(it->when)},
          {"kind", V(it->published ? "published" : "applied_observed")},
          {"factor", number(it->factor)}});
      const auto bytes = content_identity::canonical_json(row).size() + 1U;
      if (used > allowance || bytes > allowance - used) {
        truncated = true;
        break;
      }
      used += bytes;
      scale_values.emplace_back(std::move(row));
    }
    auto value = make(0);
    std::size_t size = 0;
    for (int pass = 0; pass < 4; ++pass) {
      const auto next = content_identity::canonical_json(value).size();
      if (next == size)
        break;
      size = next;
      value = make(size);
    }
    if (content_identity::canonical_json(value).size() != size ||
        size > allowance)
      return error("trace final encoded size exceeds its allowance");
    return core::Result<V>::Ok(std::move(value));
  } catch (...) {
    ++failures_;
    return error("trace snapshot capture or encoding failed");
  }
}
} // namespace manyforge::ros2_control_execution
