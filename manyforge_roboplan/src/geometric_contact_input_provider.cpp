// Copyright 2026 Flexin Group SRL
// Licensed under the Apache License, Version 2.0.

#include <manyforge_roboplan/geometric_contact_input_provider.hpp>

#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {
core::ErrorDTO unavailable(std::string reason) {
  return {"io_source_unavailable", "Geometric contact observation is unavailable",
          {{"reason", std::move(reason)}}};
}

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
  return value.size() >= prefix.size() &&
         value.substr(0U, prefix.size()) == prefix;
}

core::CollisionPairId pair(core::CollisionBodyId left,
                           core::CollisionBodyId right) {
  if (right < left)
    std::swap(left, right);
  return {std::move(left), std::move(right)};
}

std::optional<std::vector<std::string>> query_joint_names(
    const GeometricContactInputBinding &binding) {
  std::vector<std::string> result;
  std::set<std::string, std::less<>> seen;
  for (const auto &publication : binding.observed_states) {
    if (!publication || publication->canonical_joint_names().empty())
      return std::nullopt;
    for (const auto &name : publication->canonical_joint_names())
      if (!seen.insert(name).second)
        return std::nullopt;
      else
        result.push_back(name);
  }
  return result.empty() ? std::nullopt
                        : std::optional<std::vector<std::string>>(
                              std::move(result));
}
} // namespace

struct GeometricContactInputProvider::State final
    : public std::enable_shared_from_this<State> {
  struct RuntimeBinding final {
    RuntimeBinding(GeometricContactInputBinding selected,
                   std::vector<std::string> selected_joint_names)
        : value(std::move(selected)),
          joint_names(std::move(selected_joint_names)),
          positions(joint_names.size()) {}

    GeometricContactInputBinding value;
    std::vector<std::string> joint_names;
    std::vector<double> positions;
    std::vector<core::CollisionPairId> endpoint_pairs;
    core::RuntimeCollisionQueryBundlePtr prepared_bundle;
    // Resume can overlap the previous generation's callback. Keep lifecycle
    // nonblocking: a newer overlapping acquisition becomes unavailable while
    // the older receipt remains fenced by IoSourceIngressSender's generation.
    std::atomic_flag query_active = ATOMIC_FLAG_INIT;
  };

  State(std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
        core::IScheduler &scheduler, GeometricContactInputBinding binding,
        std::vector<std::string> joint_names,
        std::optional<std::uint64_t> generation)
      : clock(clock), scheduler(scheduler),
        sender(std::move(ingress), clock, binding.source_id,
               binding.incarnation, generation),
        runtime(std::make_shared<RuntimeBinding>(std::move(binding),
                                                 std::move(joint_names))),
        generation(generation.value_or(0U)), enabled(!generation) {}

  bool Prepare(RuntimeBinding &selected,
               const core::RuntimeCollisionQueryBundlePtr &bundle) {
    if (!bundle || !bundle->world || !bundle->lookup_order_valid)
      return false;
    const auto &capability = selected.value.collision_engine->capability();
    if (bundle->world->bodies.size() > capability.runtime_query_max_world_bodies ||
        bundle->body_freshness.size() > capability.runtime_query_max_world_bodies)
      return false;
    if (selected.value.collision_engine->prepare_runtime_snapshot(*bundle->world) !=
        core::RuntimeCollisionQueryPrepareStatus::kReady)
      return false;

    std::vector<core::CollisionBodyId> endpoint_bodies;
    for (const auto &binding : selected.value.endpoint.contact_body_bindings)
      endpoint_bodies.insert(endpoint_bodies.end(),
                             binding.collision_body_ids.begin(),
                             binding.collision_body_ids.end());
    std::sort(endpoint_bodies.begin(), endpoint_bodies.end());
    endpoint_bodies.erase(
        std::unique(endpoint_bodies.begin(), endpoint_bodies.end()),
        endpoint_bodies.end());
    if (endpoint_bodies.empty())
      return false;

    const auto target_prefix =
        std::string("scene/") +
        selected.value.target_scene_resource_id.value() + "/shape/";
    std::vector<core::CollisionBodyId> target_bodies;
    for (const auto &body : bundle->world->bodies)
      if (starts_with(body.body_id, target_prefix))
        target_bodies.push_back(body.body_id);
    if (target_bodies.empty())
      return false;
    for (const auto &body : endpoint_bodies)
      if (std::none_of(bundle->world->bodies.begin(), bundle->world->bodies.end(),
                       [&](const auto &candidate) {
                         return candidate.body_id == body;
                       }))
        return false;

    selected.endpoint_pairs.clear();
    if (endpoint_bodies.size() >
        capability.runtime_query_max_collision_pairs / target_bodies.size())
      return false;
    selected.endpoint_pairs.reserve(endpoint_bodies.size() * target_bodies.size());
    for (const auto &endpoint_body : endpoint_bodies)
      for (const auto &target_body : target_bodies)
        selected.endpoint_pairs.push_back(pair(endpoint_body, target_body));
    std::sort(selected.endpoint_pairs.begin(), selected.endpoint_pairs.end());
    selected.endpoint_pairs.erase(
        std::unique(selected.endpoint_pairs.begin(), selected.endpoint_pairs.end()),
        selected.endpoint_pairs.end());
    selected.prepared_bundle = bundle;
    return !selected.endpoint_pairs.empty();
  }

  void PublishUnavailable(const std::shared_ptr<RuntimeBinding> &selected,
                          core::IoSourceIngressAcquisitionReceipt receipt,
                          std::string reason) {
    std::vector<core::IoSignalUpdateDTO> updates;
    updates.reserve(selected->value.signals.size());
    for (const auto &signal : selected->value.signals)
      updates.push_back({signal.signal_id, core::IoSampleValidity::kUnavailable,
                         std::nullopt, unavailable(reason)});
    sender.Receive(std::move(updates), std::move(receipt));
  }

  void Sample(
      std::uint64_t expected_generation,
      std::optional<core::TimePoint> scheduled_deadline = std::nullopt) {
    std::shared_ptr<RuntimeBinding> selected;
    core::IoSourceIncarnation incarnation(std::string{});
    {
      std::lock_guard lock(mutex);
      if (retired || !enabled || generation != expected_generation)
        return;
      selected = runtime;
      incarnation = selected->value.incarnation;
      scheduled.reset();
    }
    const auto receipt =
        sender.CaptureAcquisition(incarnation, expected_generation);
    const auto now = receipt.received_at_ns;
    const auto cadence_anchor = scheduled_deadline.value_or(now);
    if (selected->query_active.test_and_set(std::memory_order_acquire)) {
      PublishUnavailable(selected, receipt, "collision_query_busy");
      ScheduleNext(expected_generation, cadence_anchor, clock.now());
      return;
    }
    struct QueryLease final {
      std::atomic_flag &active;
      ~QueryLease() { active.clear(std::memory_order_release); }
    } query_lease{selected->query_active};
    std::size_t position_index = 0U;
    auto state_time = now;
    for (const auto &publication : selected->value.observed_states) {
      auto observed = publication->acquire(now);
      if (!observed || !observed.complete() ||
          observed.positions_rad().empty() ||
          observed.positions_rad().size() !=
              publication->canonical_joint_names().size() ||
          position_index > selected->positions.size() ||
          observed.positions_rad().size() >
              selected->positions.size() - position_index ||
          observed.received_timestamp_ns() < 0 ||
          observed.received_timestamp_ns() > now ||
          (observed.freshness_budget_ns() > 0 &&
           now - observed.received_timestamp_ns() >
               observed.freshness_budget_ns())) {
        PublishUnavailable(selected, receipt,
                           "observed_state_missing_or_stale");
        ScheduleNext(expected_generation, cadence_anchor, clock.now());
        return;
      }
      std::copy(observed.positions_rad().begin(),
                observed.positions_rad().end(),
                selected->positions.begin() +
                    static_cast<std::ptrdiff_t>(position_index));
      position_index += observed.positions_rad().size();
      state_time = std::min(state_time, observed.received_timestamp_ns());
    }
    if (position_index != selected->positions.size()) {
      PublishUnavailable(selected, receipt,
                         "observed_state_missing_or_stale");
      ScheduleNext(expected_generation, cadence_anchor, clock.now());
      return;
    }

    auto bundle =
        selected->value.collision_publisher
            ->published_runtime_collision_query_bundle();
    if (!bundle || !bundle->world ||
        core::check_runtime_dynamic_body_freshness(
            *bundle->world, bundle->body_freshness, now,
            selected->value.dynamic_body_max_age_cap_ns.value_or(0), true)
                .status != core::RuntimeDynamicBodyFreshnessStatus::kFresh) {
      PublishUnavailable(selected, receipt, "collision_geometry_missing_or_stale");
      ScheduleNext(expected_generation, cadence_anchor, clock.now());
      return;
    }
    if (selected->prepared_bundle != bundle && !Prepare(*selected, bundle)) {
      PublishUnavailable(selected, receipt, "collision_geometry_unresolved");
      ScheduleNext(expected_generation, cadence_anchor, clock.now());
      return;
    }

    const auto result = selected->value.collision_engine->check_runtime(
        *bundle->world,
        {selected->positions, state_time,
         selected->value.query_timeout_ns,
         core::CollisionDistanceRequestMode::kNone, {}});
    const auto pair_bound =
        selected->value.collision_engine->capability()
            .runtime_query_max_collision_pairs;
    if (result.status == core::RuntimeCollisionQueryStatus::kInconclusive ||
        result.failure_reason != core::RuntimeCollisionQueryFailureReason::kNone ||
        result.collision_pairs.size() > pair_bound ||
        std::any_of(result.collision_pairs.begin(), result.collision_pairs.end(),
                    [](const auto &entry) { return entry.pair == nullptr; })) {
      PublishUnavailable(selected, receipt, "collision_query_inconclusive");
      ScheduleNext(expected_generation, cadence_anchor, clock.now());
      return;
    }
    const bool contact = std::any_of(
        result.collision_pairs.begin(), result.collision_pairs.end(),
        [&](const auto &entry) {
          return std::binary_search(selected->endpoint_pairs.begin(),
                                    selected->endpoint_pairs.end(), *entry.pair);
        });
    std::vector<core::IoSignalUpdateDTO> updates;
    updates.reserve(selected->value.signals.size());
    for (const auto &signal : selected->value.signals)
      updates.push_back({signal.signal_id, core::IoSampleValidity::kValid,
                         contact, std::nullopt});
    sender.Receive(std::move(updates), receipt);
    ScheduleNext(expected_generation, cadence_anchor, clock.now());
  }

  void ScheduleNext(std::uint64_t expected_generation,
                    core::TimePoint cadence_anchor,
                    core::TimePoint completed_at) {
    std::lock_guard lock(mutex);
    if (retired || !enabled || generation != expected_generation ||
        scheduled || selected_period_invalid())
      return;
    const auto period = runtime->value.sample_period_ns;
    if (cadence_anchor > std::numeric_limits<core::TimePoint>::max() - period)
      return;
    auto deadline = cadence_anchor + period;
    if (completed_at >= deadline) {
      const auto elapsed = completed_at - cadence_anchor;
      const auto periods = elapsed / period + 1;
      if (periods >
          (std::numeric_limits<core::TimePoint>::max() - cadence_anchor) /
              period)
        return;
      deadline = cadence_anchor + periods * period;
    }
    std::weak_ptr<State> weak = shared_from_this();
    scheduled = scheduler.schedule(
        deadline,
        [weak, expected_generation, deadline] {
          if (auto self = weak.lock())
            self->Sample(expected_generation, deadline);
        },
        core::SchedulerPriority::Monitoring);
  }

  bool selected_period_invalid() const noexcept {
    return runtime->value.sample_period_ns <= 0;
  }

  core::IClock &clock;
  core::IScheduler &scheduler;
  core::IoSourceIngressSender sender;
  mutable std::mutex mutex;
  std::shared_ptr<RuntimeBinding> runtime;
  std::optional<core::ScheduleHandle> scheduled;
  std::optional<GeometricContactInputBaseline> resume_baseline;
  std::uint64_t resume_baseline_generation{};
  std::uint64_t generation{};
  bool enabled{};
  bool retired{};
};

core::Result<std::unique_ptr<GeometricContactInputProvider>>
GeometricContactInputProvider::Create(
    std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
    core::IScheduler &scheduler, GeometricContactInputBinding binding,
    std::optional<std::uint64_t> prepared_driver_generation) {
  using Reply =
      core::Result<std::unique_ptr<GeometricContactInputProvider>>;
  if (!ingress || binding.source_id.value().empty() ||
      binding.incarnation.empty() || binding.signals.empty() ||
      binding.endpoint.frame.contact_body_role_ids.empty() ||
      binding.endpoint.contact_body_bindings.empty() ||
      binding.target_scene_resource_id.value().empty() ||
      binding.observed_states.empty() || !binding.collision_publisher ||
      !binding.collision_engine || binding.sample_period_ns <= 0 ||
      binding.query_timeout_ns <= 0 ||
      binding.sample_period_ns >
          std::numeric_limits<core::Duration>::max() -
              binding.query_timeout_ns ||
      (prepared_driver_generation && *prepared_driver_generation == 0U))
    return Reply::Err(unavailable("configuration_invalid"));
  auto joint_names = query_joint_names(binding);
  if (!joint_names || binding.collision_engine->prepare_runtime_query(
                          *joint_names) !=
      core::RuntimeCollisionQueryPrepareStatus::kReady)
    return Reply::Err(unavailable("collision_query_preparation_failed"));
  std::set<core::IoSignalId> seen;
  for (const auto &signal : binding.signals)
    if (!seen.insert(signal.signal_id).second ||
        signal.source_id != binding.source_id ||
        signal.direction != core::IoSignalDirection::kInput ||
        signal.value_type != core::IoSignalValueType::kBoolean ||
        signal.update_mode != core::IoSignalUpdateMode::kSampled ||
        signal.transition_capability !=
            core::IoTransitionCapability::kSampledOnly ||
        signal.freshness.mode != core::IoFreshnessMode::kSampleAge ||
        !signal.freshness.maximum_age_ns ||
        *signal.freshness.maximum_age_ns <= 0 ||
        signal.liveness != core::IoSignalLivenessMode::kPeriodic ||
        !core::ComputeIoSignalDescriptorDigest(signal).ok())
      return Reply::Err(unavailable("signal_contract_invalid"));
  auto state = std::make_shared<State>(
      std::move(ingress), clock, scheduler, std::move(binding),
      std::move(*joint_names),
      prepared_driver_generation);
  return Reply::Ok(std::unique_ptr<GeometricContactInputProvider>(
      new GeometricContactInputProvider(std::move(state))));
}

GeometricContactInputProvider::GeometricContactInputProvider(
    std::shared_ptr<State> state)
    : state_(std::move(state)) {}
GeometricContactInputProvider::~GeometricContactInputProvider() { Shutdown(); }

bool GeometricContactInputProvider::Pause(std::uint64_t generation) {
  std::optional<core::ScheduleHandle> handle;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired || generation == 0U || generation < state_->generation)
      return false;
    if (generation == state_->generation)
      return !state_->enabled;
    state_->generation = generation;
    state_->enabled = false;
    state_->resume_baseline.reset();
    state_->resume_baseline_generation = 0U;
    handle = std::exchange(state_->scheduled, std::nullopt);
  }
  if (handle)
    state_->scheduler.cancel(*handle);
  return state_->sender.Pause(generation);
}

core::Result<GeometricContactInputBaseline>
GeometricContactInputProvider::Resume(std::uint64_t generation) {
  using Reply = core::Result<GeometricContactInputBaseline>;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired || generation == 0U || generation <= state_->generation)
      return generation == state_->generation && state_->enabled &&
                     state_->resume_baseline &&
                     state_->resume_baseline_generation == generation
                 ? Reply::Ok(*state_->resume_baseline)
                 : Reply::Err(unavailable("resume_stale_or_disabled"));
    state_->generation = generation;
    state_->enabled = true;
    state_->resume_baseline.reset();
    state_->resume_baseline_generation = 0U;
  }
  const auto before = state_->sender.status().last_enqueued_source_sequence;
  if (!state_->sender.Resume(generation)) {
    std::lock_guard lock(state_->mutex);
    state_->enabled = false;
    return Reply::Err(unavailable("resume_ingress_rejected"));
  }
  state_->Sample(generation);
  const auto after = state_->sender.status().last_enqueued_source_sequence;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->retired && state_->enabled &&
        state_->generation == generation && after > before) {
      state_->resume_baseline = GeometricContactInputBaseline{
          state_->runtime->value.incarnation, after};
      state_->resume_baseline_generation = generation;
      return Reply::Ok(*state_->resume_baseline);
    }
  }
  static_cast<void>(AbortResume(generation));
  return Reply::Err(unavailable("initial_acquisition_not_enqueued"));
}

bool GeometricContactInputProvider::AbortResume(std::uint64_t generation) {
  std::optional<core::ScheduleHandle> handle;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired || generation == 0U ||
        generation != state_->generation)
      return false;
    state_->enabled = false;
    state_->resume_baseline.reset();
    state_->resume_baseline_generation = 0U;
    handle = std::exchange(state_->scheduled, std::nullopt);
  }
  const bool aborted = state_->sender.AbortResume(generation);
  if (handle)
    state_->scheduler.cancel(*handle);
  return aborted;
}

core::Result<void> GeometricContactInputProvider::RebindIngress(
    std::shared_ptr<core::IIoSignalIngress> ingress,
    GeometricContactInputBinding binding, std::uint64_t generation) {
  if (!ingress || generation == 0U || binding.incarnation.empty() ||
      binding.observed_states.empty() || !binding.collision_publisher ||
      !binding.collision_engine)
    return core::Result<void>::Err(unavailable("rebind_invalid"));
  auto joint_names = query_joint_names(binding);
  if (!joint_names || binding.collision_engine->prepare_runtime_query(
                          *joint_names) !=
          core::RuntimeCollisionQueryPrepareStatus::kReady)
    return core::Result<void>::Err(unavailable("rebind_invalid"));
  std::optional<core::ScheduleHandle> handle;
  bool enabled = false;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired || generation <= state_->generation ||
        binding.source_id != state_->runtime->value.source_id ||
        binding.signals != state_->runtime->value.signals ||
        binding.target_scene_resource_id !=
            state_->runtime->value.target_scene_resource_id)
      return core::Result<void>::Err(unavailable("rebind_stale_or_changed"));
    handle = std::exchange(state_->scheduled, std::nullopt);
    enabled = state_->enabled;
  }
  if (handle)
    state_->scheduler.cancel(*handle);
  if (!state_->sender.RebindIngress(std::move(ingress), binding.incarnation,
                                    generation))
    return core::Result<void>::Err(unavailable("rebind_ingress_failed"));
  {
    std::lock_guard lock(state_->mutex);
    state_->runtime = std::make_shared<State::RuntimeBinding>(
        std::move(binding), std::move(*joint_names));
    state_->generation = generation;
    state_->resume_baseline.reset();
    state_->resume_baseline_generation = 0U;
  }
  if (enabled)
    state_->Sample(generation);
  return core::Result<void>::Ok();
}

void GeometricContactInputProvider::Shutdown() {
  std::optional<core::ScheduleHandle> handle;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->retired)
      return;
    state_->retired = true;
    state_->enabled = false;
    state_->resume_baseline.reset();
    state_->resume_baseline_generation = 0U;
    handle = std::exchange(state_->scheduled, std::nullopt);
  }
  if (handle)
    state_->scheduler.cancel(*handle);
  state_->sender.Shutdown();
}

} // namespace manyforge::roboplan_backend
