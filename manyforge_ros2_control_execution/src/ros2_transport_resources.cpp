// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include "scene_projection_worker.hpp"
#include <manyforge_core/time/stuck_drain_wait.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace manyforge::ros2_control_execution {

struct Ros2TransportResources::RetirementRecord {
  RetirementRecord(std::shared_ptr<void> value, rclcpp::CallbackGroup::SharedPtr group)
      : time_critical_group(std::move(group)), entity(std::move(value)) {}
  // Declared before the entity so it outlives it: an action client's deleter
  // removes its waitable from this group.
  rclcpp::CallbackGroup::SharedPtr time_critical_group;
  std::shared_ptr<void> entity;
  // Removal fence this record waits for (time-critical records only).
  std::uint64_t fence = 0;
  std::shared_ptr<RetirementRecord> next;
};

struct Ros2TransportResources::RetirementNotifier {
  std::shared_ptr<std::function<void()>> notify;
  std::atomic<bool> pending{false};
  std::atomic<bool> active{true};

  void trigger() noexcept {
    if (!active.load()) return;
    pending.store(true, std::memory_order_release);
    try {
      (*notify)();
      return;
    } catch (const std::exception &error) {
      if (!active.load()) return;
      std::fprintf(stderr, "ros_transport_retirement_notification_failed: %s\n", error.what());
    } catch (...) {
      if (!active.load()) return;
      std::fprintf(stderr, "ros_transport_retirement_notification_failed: unknown exception\n");
    }
  }
};

// Lock order: `finalizer_mutex` -> `mutex`, never the reverse.
// `mutex` guards only the pending and fenced lists and the joined transition;
// no entity is destroyed while it is held, and callbacks take it only to push
// a record. `fenced` holds time-critical records whose group the owner thread
// removed from the time-critical executor, until that thread acknowledges a
// rebuild without it (OD-66): only then can the owner hold the last reference.
// `finalizer_mutex` serializes post-join finalization (and post-join creation)
// for this node; this owner's callbacks never take it (they run only before
// join). It is recursive because a finalizer can release another borrow.
struct Ros2TransportResources::RetirementState {
  std::recursive_mutex finalizer_mutex;
  std::recursive_mutex mutex;
  std::shared_ptr<RetirementNotifier> notifier;
  std::shared_ptr<RetirementRecord> pending;
  std::shared_ptr<RetirementRecord> fenced;
  std::atomic<bool> joined{false};
};

struct Ros2TransportResources::ContextWatch {
  explicit ContextWatch(std::shared_ptr<rclcpp::Context> selected)
      : context(std::move(selected)), valid(std::make_shared<std::atomic<bool>>(true)) {
    const std::weak_ptr<std::atomic<bool>> weak = valid;
    callback = context->add_on_shutdown_callback([weak] {
      if (const auto state = weak.lock()) state->store(false, std::memory_order_release);
    });
    if (!context->is_valid()) valid->store(false, std::memory_order_release);
  }
  ~ContextWatch() {
    try { (void)context->remove_on_shutdown_callback(callback); } catch (...) {}
  }
  std::shared_ptr<rclcpp::Context> context;
  std::shared_ptr<std::atomic<bool>> valid;
  rclcpp::OnShutdownCallbackHandle callback;
};

Ros2TransportResources::Ros2TransportResources(
    std::string node_name, std::string node_namespace,
    std::size_t executor_thread_count, ContextProvider context_provider)
    : owns_private_context_(!context_provider) {
  retirement_state_ = std::make_shared<RetirementState>();
  if (context_provider) {
    context_ = context_provider();
    if (!context_ || !context_->is_valid())
      throw std::invalid_argument("ROS transport requires a valid supplied context");
  } else {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);
  }
  context_watch_ = std::make_unique<ContextWatch>(context_);
  rclcpp::NodeOptions options;
  options.context(context_).use_clock_thread(false);
  node_ = std::make_shared<rclcpp::Node>(std::move(node_name),
                                         std::move(node_namespace), options);
  owner_stall_snapshot_ = std::make_shared<OwnerStallSnapshot>();
  owner_stall_snapshot_->owner = node_->get_fully_qualified_name();
  // Storing the first fault must not allocate while a caller is reporting it.
  owner_stall_snapshot_->task.reserve(255);
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context_;
  // The thread-count argument remains for source compatibility with callers.
  // Each transport now owns one stock executor thread.
  (void)executor_thread_count;
  executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>(
      executor_options);
  dispatch_guard_ = std::make_shared<rclcpp::GuardCondition>(context_);
  dispatch_trigger_ = std::make_shared<std::function<void()>>(
      [guard = dispatch_guard_] { guard->trigger(); });
  retirement_state_->notifier = std::make_shared<RetirementNotifier>();
  retirement_state_->notifier->notify = dispatch_trigger_;
  dispatch_waitable_ =
      std::make_shared<rclcpp::executors::ExecutorNotifyWaitable>(
          [this] { execute_dispatch(); }, context_);
  dispatch_waitable_->add_guard_condition(dispatch_guard_);
  node_->get_node_waitables_interface()->add_waitable(dispatch_waitable_, nullptr);
  executor_->add_node(node_);
}

Ros2TransportResources::Ros2TransportResources(
    std::string node_name, std::string node_namespace,
    std::size_t executor_thread_count,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string arm_group_id, core::IClock &clock,
    ContextProvider context_provider)
    : Ros2TransportResources(std::move(node_name), std::move(node_namespace),
                             executor_thread_count, std::move(context_provider)) {
  scene_projection_worker_ = std::make_unique<detail::SceneProjectionWorker>(
      std::move(execution_state_sink), std::move(arm_group_id), clock);
}

std::shared_ptr<Ros2TransportResources> Ros2TransportResources::create(
    std::string node_name, std::string node_namespace, std::size_t executor_thread_count,
    ContextProvider context_provider) {
  return std::shared_ptr<Ros2TransportResources>(new Ros2TransportResources(
      std::move(node_name), std::move(node_namespace), executor_thread_count,
      std::move(context_provider)));
}
std::shared_ptr<Ros2TransportResources> Ros2TransportResources::create(
    std::string node_name, std::string node_namespace, std::size_t executor_thread_count,
    std::shared_ptr<core::IExecutionStateSink> sink, std::string arm_group_id,
    core::IClock &clock, ContextProvider context_provider) {
  return std::shared_ptr<Ros2TransportResources>(new Ros2TransportResources(
      std::move(node_name), std::move(node_namespace), executor_thread_count,
      std::move(sink), std::move(arm_group_id), clock, std::move(context_provider)));
}
Ros2TransportResources::~Ros2TransportResources() {
  // Final release belongs off the owner: its destructor may have to join it.
  assert(!on_owner_thread() && !on_time_critical_thread());
  (void)quiesce_impl();
}

bool Ros2TransportResources::cached_context_valid() const noexcept {
  return context_watch_ && context_watch_->valid->load(std::memory_order_acquire);
}

std::shared_ptr<Ros2TransportResources::RetirementRecord>
Ros2TransportResources::make_retirement_record(
    std::shared_ptr<void> entity, rclcpp::CallbackGroup::SharedPtr time_critical_group) {
  return std::make_shared<RetirementRecord>(std::move(entity),
                                            std::move(time_critical_group));
}

void Ros2TransportResources::release_retirement(
    const std::shared_ptr<RetirementState> &state,
    std::shared_ptr<RetirementRecord> record) noexcept {
  std::shared_ptr<RetirementNotifier> notifier;
  {
    std::unique_lock lock(state->mutex);
    if (!state->joined.load(std::memory_order_acquire)) {
      record->next = std::move(state->pending);
      state->pending = std::move(record);
      notifier = state->notifier;
    }
  }
  if (notifier) {
    notifier->trigger();
    return;
  }
  // Post-join: serialize finalizers for the same node, including reentry,
  // without holding the retirement mutex across the destruction.
  std::lock_guard finalizer(state->finalizer_mutex);
  record.reset();
}

void Ros2TransportResources::destroy_chain(
    std::shared_ptr<RetirementRecord> pending) noexcept {
  while (pending) {
    auto next = std::move(pending->next);
    pending.reset();
    pending = std::move(next);
  }
}

void Ros2TransportResources::drain_retirements(const bool executors_joined) noexcept {
  std::shared_ptr<RetirementRecord> pending;
  std::shared_ptr<RetirementRecord> destroy;
  const auto acked = time_critical_fence_acked_.load(std::memory_order_acquire);
  {
    std::lock_guard lock(retirement_state_->mutex);
    pending.swap(retirement_state_->pending);
    // Fenced records whose fence the time-critical thread acknowledged (or
    // all of them once both threads are joined) are now owner-only.
    for (auto *link = &retirement_state_->fenced; *link;) {
      if (executors_joined || (*link)->fence <= acked) {
        auto ready = std::move(*link);
        *link = std::move(ready->next);
        ready->next = std::move(destroy);
        destroy = std::move(ready);
      } else {
        link = &(*link)->next;
      }
    }
  }
  // A running time-critical executor may hold a strong reference to any
  // entity of its groups while it waits (rclcpp's wait set acquires
  // ownership for the wait). Remove the group first and destroy the record
  // only after that thread acknowledged a rebuild without it.
  const bool fence_required = !executors_joined && time_critical_executor_ &&
      !time_critical_spin_finished_.load(std::memory_order_acquire);
  std::shared_ptr<RetirementRecord> fence;
  while (pending) {
    auto next = std::move(pending->next);
    if (fence_required && pending->time_critical_group) {
      try {
        time_critical_executor_->remove_callback_group(pending->time_critical_group, true);
      } catch (...) {
        // Not associated (or the notify failed): the fence still orders the
        // release after the next acknowledged rebuild, or after join.
      }
      pending->next = std::move(fence);
      fence = std::move(pending);
    } else {
      pending->next = std::move(destroy);
      destroy = std::move(pending);
    }
    pending = std::move(next);
  }
  if (fence) {
    // Requested only after every removal above is registered.
    const auto id = time_critical_fence_requested_.fetch_add(1, std::memory_order_acq_rel) + 1U;
    {
      std::lock_guard lock(retirement_state_->mutex);
      while (fence) {
        auto next = std::move(fence->next);
        fence->fence = id;
        fence->next = std::move(retirement_state_->fenced);
        retirement_state_->fenced = std::move(fence);
        fence = std::move(next);
      }
    }
    try {
      time_critical_fence_guard_->trigger();
    } catch (...) {
      // The records stay fenced until a later acknowledgement or the join.
    }
  }
  // Destroyed with the retirement mutex released.
  destroy_chain(std::move(destroy));
}

void Ros2TransportResources::execute_time_critical_fence() {
  // Runs on the time-critical thread. An execution in a later wait cycle than
  // the one that armed a fence proves that rclcpp rebuilt the wait set (at the
  // start of that cycle's wait_for_work) without the removed groups, so this
  // thread holds no reference to their entities any more.
  bool acknowledged = false;
  if (time_critical_fence_acked_.load(std::memory_order_relaxed) < time_critical_fence_armed_) {
    time_critical_fence_acked_.store(time_critical_fence_armed_, std::memory_order_release);
    acknowledged = true;
  }
  const auto requested = time_critical_fence_requested_.load(std::memory_order_acquire);
  if (time_critical_fence_armed_ < requested) {
    time_critical_fence_armed_ = requested;
    time_critical_fence_guard_->trigger();
  }
  if (acknowledged)
    retirement_state_->notifier->trigger();
}

void Ros2TransportResources::enable_time_critical_callbacks() {
  const auto refuse_started = [this] {
    if (started() || starting_ || quiesced_)
      throw std::logic_error(
          "ROS transport time-critical callbacks must be enabled before start");
  };
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (time_critical_executor_) return;
    refuse_started();
  }
  // Built without the lifecycle lock: no lock is held across these rclcpp
  // calls. None of them creates an rcl node entity (callback groups,
  // guard conditions and the executor are context-level objects).
  rclcpp::ExecutorOptions options;
  options.context = context_;
  auto executor = std::make_shared<rclcpp::executors::SingleThreadedExecutor>(options);
  auto group = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive,
                                            false);
  auto guard = std::make_shared<rclcpp::GuardCondition>(context_);
  auto waitable = std::make_shared<rclcpp::executors::ExecutorNotifyWaitable>(
      [this] { execute_time_critical_fence(); }, context_);
  waitable->add_guard_condition(guard);
  node_->get_node_waitables_interface()->add_waitable(waitable, group);
  executor->add_callback_group(group, node_->get_node_base_interface());
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (time_critical_executor_) return;  // A concurrent enable won; ours is unused.
  refuse_started();
  time_critical_fence_guard_ = std::move(guard);
  time_critical_fence_waitable_ = std::move(waitable);
  time_critical_fence_group_ = std::move(group);
  time_critical_executor_ = std::move(executor);
}

rclcpp::CallbackGroup::SharedPtr
Ros2TransportResources::create_time_critical_callback_group() {
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    executor = time_critical_executor_;
    if (!executor && (started() || starting_ || quiesced_))
      return {};
  }
  if (!executor) {
    enable_time_critical_callbacks();
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    executor = time_critical_executor_;
  }
  // A CallbackGroup and its notify guard condition are not rcl node entities.
  // add_callback_group is safe while the time-critical executor spins: the
  // group's guard condition joins that executor's notify waitable, and each
  // entity later created in the group triggers it (rclcpp NodeTopics /
  // NodeWaitables), so that executor rebuilds and serves the new entity.
  auto group = node_->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive,
                                            false);
  executor->add_callback_group(group, node_->get_node_base_interface());
  return group;
}

bool Ros2TransportResources::on_time_critical_thread() {
  // Like on_owner_thread(): an exited thread's id may be reused.
  std::lock_guard<std::mutex> lock(dispatch_mutex_);
  return !time_critical_spin_finished_.load(std::memory_order_acquire) &&
         time_critical_thread_id_ != std::thread::id{} &&
         time_critical_thread_id_ == std::this_thread::get_id();
}

void Ros2TransportResources::run_time_critical_thread() noexcept {
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    time_critical_thread_id_ = std::this_thread::get_id();
  }
  bool notify_fault = false;
  const auto fail = [&](const char *text) noexcept {
    executor_failure_started_.store(true, std::memory_order_release);
    (void)executor_failure_source_.request_stop();
    close_dispatch();
    notify_fault = record_executor_fault(text);
    // The whole owner stops: its owner thread exits through its own path,
    // releasing any caller waiting for it.
    try { executor_->cancel(); } catch (...) {}
  };
  try {
    time_critical_executor_->spin();
  } catch (const std::exception &error) {
    fail(error.what());
  } catch (...) {
    fail("unknown_non_std_exception");
  }
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    time_critical_spin_finished_.store(true, std::memory_order_release);
  }
  dispatch_cv_.notify_all();
  if (notify_fault && executor_fault_observer_)
    executor_fault_observer_("ros_transport_executor_failed");
}

bool Ros2TransportResources::stop_time_critical_thread(
    std::optional<std::chrono::steady_clock::time_point> deadline) noexcept {
  if (!time_critical_thread_.joinable()) return true;
  // cancel() before spin() has started is overwritten by spin(); repeat it
  // until the thread reports that its spin returned.
  std::unique_lock<std::mutex> lock(dispatch_mutex_);
  while (!time_critical_spin_finished_.load(std::memory_order_acquire)) {
    lock.unlock();
    try { time_critical_executor_->cancel(); } catch (...) {}
    lock.lock();
    if (deadline && std::chrono::steady_clock::now() >= *deadline) return false;
    dispatch_cv_.wait_for(lock, std::chrono::milliseconds(1), [this] {
      return time_critical_spin_finished_.load(std::memory_order_acquire);
    });
  }
  lock.unlock();
  time_critical_thread_.join();
  return true;
}

detail::SceneProjectionWorker &
Ros2TransportResources::projection_worker() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (!scene_projection_worker_)
    throw std::logic_error("ROS transport has no scene projection worker");
  return *scene_projection_worker_;
}

std::unique_ptr<detail::SceneProjectionWorker>
Ros2TransportResources::take_projection_worker() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (started() || quiesced_ || spin_finished_.load(std::memory_order_acquire) ||
      !scene_projection_worker_)
    throw std::logic_error("ROS transport projection worker cannot be transferred");
  return std::move(scene_projection_worker_);
}

void Ros2TransportResources::start() {
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (started() || starting_ || quiesced_ ||
        spin_finished_.load(std::memory_order_acquire))
      throw std::logic_error(
          "ROS transport cannot be started twice or after quiescence");
    starting_ = true;
  }
  struct StartGuard final {
    Ros2TransportResources* owner;
    ~StartGuard() {
      {
        std::lock_guard<std::mutex> lock(owner->lifecycle_mutex_);
        owner->starting_ = false;
      }
      owner->quiesce_cv_.notify_all();
    }
  } starting{this};
  {
    std::unique_lock<std::mutex> dispatch_lock(dispatch_mutex_);
    if (dispatch_stopping_)
      throw core::TypedError("component_not_ready", "ROS transport is closing",
                             {{"reason", "owner_closing"}});
    dispatch_starting_ = true;
    dispatch_cv_.wait(dispatch_lock, [this] {
      return foreign_invocations_ == 0 || dispatch_stopping_;
    });
    if (dispatch_stopping_) {
      dispatch_starting_ = false;
      throw core::TypedError("component_not_ready", "ROS transport is closing",
                             {{"reason", "owner_closing"}});
    }
  }
  try {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (quiesced_)
      throw core::TypedError("component_not_ready", "ROS transport is closing",
                             {{"reason", "owner_closing"}});
    spin_thread_ = std::thread([this]() {
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex_);
      owner_thread_id_ = std::this_thread::get_id();
    }
    bool notify_fault = false;
    try {
      executor_->spin();
    } catch (const std::exception &error) {
      executor_failure_started_.store(true, std::memory_order_release);
      (void)executor_failure_source_.request_stop();
      close_dispatch();
      notify_fault = record_executor_fault(error.what());
    } catch (...) {
      executor_failure_started_.store(true, std::memory_order_release);
      (void)executor_failure_source_.request_stop();
      close_dispatch();
      notify_fault = record_executor_fault("unknown_non_std_exception");
    }
    // The whole owner stops with either thread (best effort here; quiesce
    // repeats the cancel until the time-critical spin has returned).
    if (executor_failure_started_.load(std::memory_order_acquire) && time_critical_executor_)
      try { time_critical_executor_->cancel(); } catch (...) {}
    // A notifier failure can be reported by a thread holding a retirement
    // lock. Signal from here instead, before waiting for that notifier and
    // without running stop callbacks under any transport lock.
    if (executor_failure_started_.load(std::memory_order_acquire))
      (void)executor_failure_source_.request_stop();
    close_dispatch();
    std::optional<std::string> notifier_fault;
    {
      std::unique_lock<std::mutex> lock(dispatch_mutex_);
      dispatch_cv_.wait(lock, [this] { return notifiers_in_flight_ == 0; });
      notifier_fault = notifier_fault_;
    }
    if (notifier_fault)
      notify_fault = record_executor_fault(notifier_fault->c_str()) || notify_fault;
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex_);
      dispatch_stopping_ = true;
      dispatch_starting_ = false;
      owner_exited_ = true;
      for (auto **slot : {&ordinary_slot_, &close_slot_}) {
        if (*slot) {
          (*slot)->done = true;
          *slot = nullptr;
        }
      }
    }
    dispatch_cv_.notify_all();
    // The observer may acquire locks held by callers waiting for this owner.
    // Publish the fault and release those callers before invoking it.
    if (notify_fault && executor_fault_observer_)
      executor_fault_observer_("ros_transport_executor_failed");
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex_);
      spin_finished_.store(true, std::memory_order_release);
    }
    dispatch_cv_.notify_all();
    });
  } catch (...) {
    {
      std::lock_guard<std::mutex> dispatch_lock(dispatch_mutex_);
      dispatch_starting_ = false;
    }
    dispatch_cv_.notify_all();
    throw;
  }
  // spin() establishes its cancellation state internally. Do not let quiesce()
  // cancel before that state exists: spin() would overwrite an early cancel.
  while (!executor_->is_spinning() &&
         !spin_finished_.load(std::memory_order_acquire))
    std::this_thread::yield();
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (quiesced_)
      throw core::TypedError("component_not_ready", "ROS transport is closing",
                             {{"reason", "owner_closing"}});
    if (spin_finished_.load(std::memory_order_acquire)) {
      spin_thread_.join();
      throw std::runtime_error("ROS transport executor exited during startup");
    }
    // OD-66: the time-critical thread starts after the owner thread spins.
    // Retirements drained before it spins are fenced and wait for it.
    if (time_critical_executor_)
      time_critical_thread_ = std::thread([this] { run_time_critical_thread(); });
    std::lock_guard<std::mutex> dispatch_lock(dispatch_mutex_);
    started_.store(true, std::memory_order_release);
    dispatch_starting_ = false;
  }
  dispatch_cv_.notify_all();
}

bool Ros2TransportResources::on_owner_thread() {
  std::lock_guard<std::mutex> lock(dispatch_mutex_);
  return !owner_exited_ && owner_thread_id_ == std::this_thread::get_id();
}

std::shared_ptr<const Ros2TransportResources::OwnerStallSnapshot>
Ros2TransportResources::owner_stall() const noexcept {
  if (!owner_stall_published_.load(std::memory_order_acquire))
    return {};
  return owner_stall_snapshot_;
}

bool Ros2TransportResources::record_owner_stall_locked() {
  if (owner_stall_published_.load(std::memory_order_relaxed))
    return false;
  const char *name = active_task_ ? active_task_->site.function_name()
                                  : owner_notification_active_ ? "owner_notification"
                                                               : "unknown";
  if (*name == '\0')
    name = "unknown_task";
  const auto bounded_name = std::string_view(name).substr(0, 255);
  owner_stall_snapshot_->task.assign(bounded_name.data(), bounded_name.size());
  owner_stall_published_.store(true, std::memory_order_release);
  return true;
}

void Ros2TransportResources::invoke(std::function<void()> work,
                                    DispatchOrder order,
                                    std::source_location site) {
  invoke_impl(std::move(work), order, site, false);
}

void Ros2TransportResources::invoke_creation(std::function<void()> work,
                                             std::source_location site) {
  invoke_impl(std::move(work), DispatchOrder::kFifo, site, true);
}

void Ros2TransportResources::invoke_impl(std::function<void()> work,
                                         DispatchOrder order,
                                         std::source_location site,
                                         bool creation) {
  const auto deadline = std::chrono::steady_clock::now() + owner_wait_bound_;
  DispatchTask task{std::move(work), false, false, {}, site};
  std::unique_lock<std::mutex> lock(dispatch_mutex_);
  const auto thread_id = std::this_thread::get_id();
  const bool inline_owner = owner_thread_id_ == thread_id && !owner_exited_;
  const bool inline_prestart = prestart_inline_threads_.contains(thread_id);
  if (creation && dispatch_stopping_ && (inline_owner || inline_prestart)) {
    const bool notifier_failed = notifier_fault_.has_value();
    lock.unlock();
    if (notifier_failed || executor_failure_started_.load(std::memory_order_acquire) ||
        executor_fault())
      throw core::TypedError("ros_transport_executor_failed",
                             "ROS transport executor failed; entity was not created");
    throw core::TypedError("component_not_ready",
                           "ROS transport is closing; entity was not created",
                           {{"reason", "owner_closing"}});
  }
  if (inline_owner) {
    auto *const previous_task = active_task_;
    active_task_ = &task;
    lock.unlock();
    try {
      task.work();
    } catch (...) {
      lock.lock();
      active_task_ = previous_task;
      lock.unlock();
      throw;
    }
    lock.lock();
    active_task_ = previous_task;
    lock.unlock();
    return;
  }
  // Startup waits for the outer pre-start call. A nested call on that same
  // thread must finish inline rather than wait for startup in return.
  if (inline_prestart) {
    lock.unlock();
    task.work();
    return;
  }
  bool observed_stall = false;
  const auto wait_for_owner = [&](auto predicate) {
    if (!observed_stall && !dispatch_cv_.wait_until(lock, deadline, predicate)) {
      const bool first = record_owner_stall_locked();
      observed_stall = true;
      if (first) {
        // Neither diagnostics nor readers hold the dispatch lock while the
        // owner continues its ordinary execution.
        lock.unlock();
        (void)std::fprintf(stderr,
                           "manyforge_fault code=component_owner_stalled "
                           "owner=%s task=%s bound_ms=%lld state=waiting_for_operator\n",
                           owner_stall_snapshot_->owner.c_str(),
                           owner_stall_snapshot_->task.c_str(),
                           static_cast<long long>(std::chrono::duration_cast<
                               std::chrono::milliseconds>(owner_wait_bound_).count()));
        lock.lock();
      }
    }
    if (observed_stall)
      dispatch_cv_.wait(lock, predicate);
  };
  wait_for_owner([this] { return !dispatch_starting_; });
  if (!started() && !dispatch_stopping_) {
    prestart_inline_threads_.insert(thread_id);
    ++foreign_invocations_;
    lock.unlock();
    try { task.work(); } catch (...) { task.error = std::current_exception(); }
    lock.lock();
    prestart_inline_threads_.erase(thread_id);
    --foreign_invocations_;
    lock.unlock();
    dispatch_cv_.notify_all();
    if (task.error)
      std::rethrow_exception(task.error);
    return;
  }
  ++foreign_invocations_;
  const bool is_close = order == DispatchOrder::kClose;
  auto &slot = is_close ? close_slot_ : ordinary_slot_;
  auto &next = is_close ? next_close_ticket_ : next_fifo_ticket_;
  auto &serving = is_close ? serving_close_ticket_ : serving_fifo_ticket_;
  const auto ticket = next++;
  wait_for_owner([&] {
    return dispatch_stopping_ || (ticket == serving && slot == nullptr);
  });
  if (!dispatch_stopping_) {
    slot = &task;
    ++serving;
    ++notifiers_in_flight_;
    lock.unlock();
    dispatch_cv_.notify_all();
    trigger_dispatch();
    lock.lock();
    wait_for_owner([&] {
      return task.done || (creation && dispatch_stopping_ && slot == &task);
    });
    if (creation && !task.done && slot == &task) {
      slot = nullptr;
      task.done = true;
      dispatch_cv_.notify_all();
    }
  } else {
    if (!creation)
      wait_for_owner([&] { return owner_exited_; });
  }
  const bool ran = task.ran;
  const bool notifier_failed = notifier_fault_.has_value();
  // A fault or teardown can release a selected slot only after spin exits.
  // The caller then uses the same locked adapter path as before start.
  lock.unlock();
  if (!ran) {
    try {
      if (creation && (notifier_failed ||
                       executor_failure_started_.load(std::memory_order_acquire) ||
                       executor_fault()))
        throw core::TypedError("ros_transport_executor_failed",
                               "ROS transport executor failed; entity was not created");
      if (creation) {
        if (!retirement_state_->joined.load(std::memory_order_acquire))
          throw core::TypedError("component_not_ready",
                                 "ROS transport is closing; entity was not created",
                                 {{"reason", "owner_closing"}});
        // Serialize post-join creation with post-join finalization.
        std::lock_guard finalizer(retirement_state_->finalizer_mutex);
        task.work();
      } else {
        task.work();
      }
    } catch (...) { task.error = std::current_exception(); }
  }
  lock.lock();
  --foreign_invocations_;
  lock.unlock();
  dispatch_cv_.notify_all();
  if (task.error)
    std::rethrow_exception(task.error);
}

bool Ros2TransportResources::trigger_dispatch() noexcept {
  bool triggered = true;
  try {
    (*dispatch_trigger_)();
  } catch (const std::exception &error) {
    triggered = false;
    executor_failure_started_.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex_);
      if (!notifier_fault_)
        notifier_fault_ = error.what();
    }
    close_dispatch();
    try { executor_->cancel(); } catch (...) {}
  } catch (...) {
    triggered = false;
    executor_failure_started_.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(dispatch_mutex_);
      if (!notifier_fault_)
        notifier_fault_ = "unknown_notifier_exception";
    }
    close_dispatch();
    try { executor_->cancel(); } catch (...) {}
  }
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    --notifiers_in_flight_;
  }
  dispatch_cv_.notify_all();
  return triggered;
}

void Ros2TransportResources::close_dispatch() noexcept {
  sdk_send_admission_closed_.store(true, std::memory_order_release);
  retirement_state_->notifier->active.store(false);
  {
    std::unique_lock lock(sdk_send_mutex_);
    sdk_send_closed_ = true;
  }
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    dispatch_stopping_ = true;
  }
  dispatch_cv_.notify_all();
}

bool Ros2TransportResources::close_dispatch_until(
    std::chrono::steady_clock::time_point deadline) noexcept {
  sdk_send_admission_closed_.store(true, std::memory_order_release);
  retirement_state_->notifier->active.store(false);
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    dispatch_stopping_ = true;
  }
  dispatch_cv_.notify_all();
  {
    std::unique_lock lock(sdk_send_mutex_, std::defer_lock);
    if (!lock.try_lock_until(deadline)) return false;
    sdk_send_closed_ = true;
  }
  return true;
}

bool Ros2TransportResources::joined() const noexcept {
  return retirement_state_->joined.load(std::memory_order_acquire);
}

void Ros2TransportResources::set_owner_notification(
    std::function<void()> work) {
  std::lock_guard<std::mutex> lock(dispatch_mutex_);
  if (started() || dispatch_starting_ || dispatch_stopping_ ||
      owner_notification_)
    throw std::logic_error(
        "ROS transport owner notification already installed or started");
  owner_notification_ = std::move(work);
}

bool Ros2TransportResources::notify_owner() noexcept {
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    if (dispatch_stopping_ || !owner_notification_)
      return false;
    owner_notification_pending_ = true;
    ++notifiers_in_flight_;
  }
  return trigger_dispatch();
}

void Ros2TransportResources::execute_dispatch() {
  DispatchTask *task = nullptr;
  bool notification = false;
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    if (close_slot_) {
      task = close_slot_;
      close_slot_ = nullptr;
    } else if (owner_notification_pending_ ||
               retirement_state_->notifier->pending.load(std::memory_order_acquire)) {
      owner_notification_pending_ = false;
      retirement_state_->notifier->pending.exchange(false, std::memory_order_acq_rel);
      notification = true;
    } else {
      task = ordinary_slot_;
      ordinary_slot_ = nullptr;
    }
    if (task || notification) {
      ++active_dispatches_;
      active_task_ = task;
      owner_notification_active_ = notification;
    }
  }
  dispatch_cv_.notify_all();
  if (!task && !notification)
    return;
  std::exception_ptr notification_error;
  try {
    if (notification) {
      drain_retirements();
      if (owner_notification_)
        owner_notification_();
    }
    else
      task->work();
  } catch (...) {
    if (task)
      task->error = std::current_exception();
    else
      notification_error = std::current_exception();
  }
  bool rearm = false;
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    if (task) {
      task->ran = true;
      task->done = true;
    }
    active_task_ = nullptr;
    owner_notification_active_ = false;
    --active_dispatches_;
    // Return to the executor between tasks, rearming any other occupied slot.
    if (close_slot_ || ordinary_slot_ || owner_notification_pending_ ||
        retirement_state_->notifier->pending.load(std::memory_order_acquire)) {
      ++notifiers_in_flight_;
      rearm = true;
    }
  }
  dispatch_cv_.notify_all();
  if (rearm)
    trigger_dispatch();
  if (notification_error)
    std::rethrow_exception(notification_error);
}

void Ros2TransportResources::set_executor_fault_observer(
    ExecutorFaultObserver observer) {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (started() || quiesced_)
    throw std::logic_error(
        "ROS transport fault observer must be set before start");
  executor_fault_observer_ = std::move(observer);
}

std::optional<std::string> Ros2TransportResources::executor_fault() const {
  std::lock_guard<std::mutex> lock(executor_fault_mutex_);
  return executor_fault_;
}

void Ros2TransportResources::check_rebuild_allowed() {
  if (!executor_fault())
    throw std::logic_error("ROS transport rebuild requires a faulted owner");
  // owner_exited_ is published before the fault observer returns. It does
  // not prove that joining from this thread is safe.
  std::lock_guard<std::mutex> lock(dispatch_mutex_);
  if ((!spin_finished_.load(std::memory_order_acquire) &&
       owner_thread_id_ == std::this_thread::get_id()) ||
      (!time_critical_spin_finished_.load(std::memory_order_acquire) &&
       time_critical_thread_id_ == std::this_thread::get_id()))
    throw core::TypedError("component_not_ready",
                           "ROS transport cannot rebuild from its owner thread");
}

void Ros2TransportResources::check_replacement_healthy() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  bool notifier_failed = false;
  bool dispatch_closed = false;
  {
    std::lock_guard<std::mutex> lock(dispatch_mutex_);
    notifier_failed = notifier_fault_.has_value();
    dispatch_closed = dispatch_stopping_ || owner_exited_;
  }
  if (notifier_failed || executor_failure_started_.load(std::memory_order_acquire) ||
      executor_fault())
    throw core::TypedError("ros_transport_executor_failed",
                           "replacement ROS transport executor failed during preparation");
  if (!started() || quiesced_ || dispatch_closed)
    throw core::TypedError("component_not_ready",
                           "replacement ROS transport is not running");
}

bool Ros2TransportResources::record_executor_fault(
    const char *exception_text) noexcept {
  (void)std::fprintf(stderr,
                     "manyforge_fault code=ros_transport_executor_failed "
                     "exception=%s\n",
                     exception_text);
  {
    std::lock_guard<std::mutex> lock(executor_fault_mutex_);
    if (executor_fault_)
      return false;
    executor_fault_ = exception_text;
  }
  return true;
}

void Ros2TransportResources::quiesce() noexcept {
  // Explicit callers pin this exact owner even if another caller returns early.
  const auto owner_pin = shared_from_this();
  (void)quiesce_impl();
}

bool Ros2TransportResources::quiesce_until(
    std::chrono::steady_clock::time_point deadline) noexcept {
  const auto owner_pin = shared_from_this();
  return quiesce_impl(deadline);
}

bool Ros2TransportResources::quiesce_impl(
    std::optional<std::chrono::steady_clock::time_point> deadline) noexcept {
  {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (joined()) return true;
    if (quiesce_active_) {
      // Preserve ordinary quiesce's early return, but bounded callers that
      // will release entities need the actual completion, not an early return.
      if (!deadline || !quiesce_cv_.wait_until(lock, *deadline,
                                               [this] { return !quiesce_active_; }))
        return false;
      if (joined()) return true;
    }
    quiesced_ = true;
    quiesce_active_ = true;
  }
  struct ActiveGuard final {
    Ros2TransportResources* owner;
    ~ActiveGuard() {
      {
        std::lock_guard<std::mutex> lock(owner->lifecycle_mutex_);
        owner->quiesce_active_ = false;
      }
      owner->quiesce_cv_.notify_all();
    }
  } active{this};
  if (deadline) {
    if (!close_dispatch_until(*deadline)) return false;
  } else {
    close_dispatch();
  }
  {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    if (deadline) {
      if (!quiesce_cv_.wait_until(lock, *deadline, [this] { return !starting_; }))
        return false;
    } else {
      quiesce_cv_.wait(lock, [this] { return !starting_; });
    }
  }
  const auto wait_dispatch = [&](std::unique_lock<std::mutex>& lock,
                                 const auto& predicate) {
    if (deadline) return dispatch_cv_.wait_until(lock, *deadline, predicate);
    dispatch_cv_.wait(lock, predicate);
    return true;
  };
  {
    std::unique_lock<std::mutex> lock(dispatch_mutex_);
    if (!wait_dispatch(lock, [this] {
          return owner_exited_ || (!ordinary_slot_ && !close_slot_ &&
                                  active_dispatches_ == 0 && notifiers_in_flight_ == 0);
        })) return false;
  }
  executor_->cancel();
  if (spin_thread_.joinable()) {
    std::unique_lock<std::mutex> lock(dispatch_mutex_);
    if (!wait_dispatch(lock, [this] {
          return spin_finished_.load(std::memory_order_acquire);
        })) return false;
    lock.unlock();
    spin_thread_.join();
  }
  {
    std::unique_lock<std::mutex> lock(dispatch_mutex_);
    owner_exited_ = true;
    dispatch_cv_.notify_all();
    if (!wait_dispatch(lock, [this] {
          return notifiers_in_flight_ == 0 && foreign_invocations_ == 0;
        })) return false;
  }
  // OD-66: join the time-critical thread too before any entity is released.
  if (!stop_time_critical_thread(deadline)) return false;
  // A bounded failure must leave the context alive. Stop any worker still
  // owned here before removing the node or shutting down its private context.
  if (deadline && scene_projection_worker_) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = now < *deadline
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now)
        : std::chrono::nanoseconds::zero();
    if (!scene_projection_worker_->stop_and_join_for(remaining)) return false;
  }
  {
    std::lock_guard finalizer(retirement_state_->finalizer_mutex);
    drain_retirements(true);
  }
  executor_->remove_node(node_);
  if (owns_private_context_)
    context_->shutdown("manyforge ROS transport shutdown");
  if (!deadline && scene_projection_worker_)
    core::WaitForDrain("ros_transport.scene_projection_worker",
                       std::chrono::seconds(1),
                       [this](std::chrono::nanoseconds bound) {
      return scene_projection_worker_->stop_and_join_for(bound);
    });
  {
    // Take the remaining records and publish join atomically under the
    // retirement mutex, then destroy them with only the finalizer held. A
    // release reentered from that destruction sees `joined` and finalizes
    // inline under the (recursive) finalizer mutex.
    std::lock_guard finalizer(retirement_state_->finalizer_mutex);
    std::shared_ptr<RetirementRecord> pending;
    std::shared_ptr<RetirementRecord> fenced;
    {
      std::lock_guard lock(retirement_state_->mutex);
      pending.swap(retirement_state_->pending);
      fenced.swap(retirement_state_->fenced);
      retirement_state_->joined.store(true, std::memory_order_release);
    }
    destroy_chain(std::move(pending));
    destroy_chain(std::move(fenced));
  }
  return true;
}

} // namespace manyforge::ros2_control_execution
