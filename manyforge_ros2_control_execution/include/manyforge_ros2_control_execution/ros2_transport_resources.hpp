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

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <rclcpp/executors/executor_notify_waitable.hpp>
#include <functional>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <source_location>
#include <shared_mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace manyforge::ros2_control_execution {
namespace detail {
class SceneProjectionWorker;
}

/// Existing ROS transport resources, retained by the host across both adapters.
/// Construct entities before start(); shutdown adapters, then quiesce(), before
/// destroying either adapter. This holder contains no command or Stop policy.
class Ros2TransportResources final
    : public std::enable_shared_from_this<Ros2TransportResources> {
  struct RetirementState;
  struct RetirementRecord;
public:
  /// Optional host-owned context, acquired only during construction. The
  /// provider may initialize it on first use; it must return a valid context.
  /// Nodes/executors remain local and quiesce() never shuts a supplied context
  /// down. The provider itself is not retained by this transport holder.
  using ContextProvider = std::function<std::shared_ptr<rclcpp::Context>()>;

  /// Construct shared-owned resources; explicit quiesce pins this exact owner.
  static std::shared_ptr<Ros2TransportResources> create(
      std::string node_name, std::string node_namespace,
      std::size_t executor_thread_count, ContextProvider context_provider = {});
  static std::shared_ptr<Ros2TransportResources> create(
      std::string node_name, std::string node_namespace,
      std::size_t executor_thread_count,
      std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
      std::string arm_group_id, core::IClock &clock,
      ContextProvider context_provider = {});
  ~Ros2TransportResources();
  Ros2TransportResources(const Ros2TransportResources &) = delete;
  Ros2TransportResources &operator=(const Ros2TransportResources &) = delete;

  rclcpp::Node::SharedPtr node() const noexcept { return node_; }
  /// Refuses transport-only holders, which own no scene projection worker.
  detail::SceneProjectionWorker &projection_worker() const;
  /// Transfer the worker to its surviving backend exactly once before start.
  /// Its new owner must stop and join it before destroying callback targets,
  /// the sink or the clock. Throws if absent, already taken, started or closed.
  std::unique_ptr<detail::SceneProjectionWorker> take_projection_worker();
  bool started() const noexcept {
    return started_.load(std::memory_order_acquire);
  }
  /// Called once, with `ros_transport_executor_failed`, when the first
  /// executor thread fails. Set before start(); used by a transport that has
  /// no health reader of its own (the host's generic I/O transport). Called at
  /// most once; must not throw. The observer contains its own faults.
  using ExecutorFaultObserver = std::function<void(std::string_view reason_code)>;
  void set_executor_fault_observer(ExecutorFaultObserver observer);
  enum class DispatchOrder { kFifo, kClose };
  struct OwnerStallSnapshot final {
    std::string owner;
    std::string task;
  };
  /// Run work exactly once, synchronously, on the owner while it is spinning.
  /// Before start and after the owner exits, run the existing locked path
  /// inline. Work exceptions propagate to the caller. Only short owner-side
  /// close work may use the reserved priority slot.
  void invoke(std::function<void()> work,
              DispatchOrder order = DispatchOrder::kFifo,
              std::source_location site = std::source_location::current());
  /// Create entities only before start or on the spinning owner; an owner
  /// fault refuses without executing the creation body.
  void invoke_creation(std::function<void()> work,
                       std::source_location site = std::source_location::current());
  /// One coalesced, non-waiting owner notification, installed before start.
  void set_owner_notification(std::function<void()> work);
  bool notify_owner() noexcept;
  /// Execute only the immediate SDK send while this exact owner epoch is
  /// open. False means the body did not run. Do not hold this guard across
  /// invoke(), waits, or locks needed by a callback/owner close.
  template <class F> bool try_sdk_send(F &&send) {
    if (sdk_send_admission_closed_.load(std::memory_order_acquire)) return false;
    std::shared_lock lock(sdk_send_mutex_);
    if (sdk_send_admission_closed_.load(std::memory_order_acquire) ||
        sdk_send_closed_ || executor_failure_started_.load(std::memory_order_acquire))
      return false;
    std::invoke(std::forward<F>(send));
    return true;
  }
  /// True only after the spin thread and post-join retirement drain finish.
  bool joined() const noexcept;
  /// OD-66: give this owner a second stock executor thread that runs only its
  /// time-critical callback groups. Idempotent; before start() only. It starts
  /// with the owner thread and is cancelled and joined with it in quiesce.
  void enable_time_critical_callbacks();
  /// A new mutually exclusive group on node() that only the time-critical
  /// thread spins (never added with the node to the owner executor). Before
  /// start() this enables that thread; afterwards an owner without one returns
  /// nullptr (the default group on the owner thread). Create the group and its
  /// entities in one creation body, and use one group per retained entity:
  /// retain_entity(entity, group) removes the group from the time-critical
  /// executor before the owner thread does the entity's final release.
  rclcpp::CallbackGroup::SharedPtr create_time_critical_callback_group();
  /// True on this owner's time-critical thread. That thread is never the
  /// owner: it runs callbacks only, never creates or destroys entities, and
  /// its callbacks never wait on invoke().
  bool on_time_critical_thread();
  /// Transfer the sole external entity reference into a copyable borrow.
  /// Its last release retires on this owner, or after join. No other entity
  /// shared_ptr may escape; retain the returned borrow throughout each use.
  /// An entity whose callbacks run in a time-critical group passes that group
  /// (one group per retained entity); a null group is the ordinary path.
  template <typename T>
  std::shared_ptr<T> retain_entity(std::shared_ptr<T> entity,
                                   rclcpp::CallbackGroup::SharedPtr time_critical_group = {}) {
    if (!entity)
      return {};
    auto *const pointer = entity.get();
    auto record = make_retirement_record(std::move(entity), std::move(time_critical_group));
    auto state = retirement_state_;
    return std::shared_ptr<T>(pointer, [state = std::move(state),
                                        record = std::move(record)](T *) mutable {
      release_retirement(state, std::move(record));
    });
  }
  /// The first owner wait that exceeded its diagnostic bound, if any. The
  /// immutable snapshot remains valid independently of the transport holder.
  std::shared_ptr<const OwnerStallSnapshot> owner_stall() const noexcept;
  /// Fully qualified owner name captured when the node was constructed.
  const std::string &owner_name() const noexcept { return owner_stall_snapshot_->owner; }
  /// Context validity observed at construction and on context shutdown.
  bool cached_context_valid() const noexcept;
  /// Within an invoked body, distinguish owner execution from the existing
  /// no-owner inline path. The latter retains the adapter's original locks.
  /// False on the time-critical thread (see on_time_critical_thread()).
  bool on_owner_thread();
  void start();
  void quiesce() noexcept;
  /// False retains the owner and context; the caller must keep them alive.
  [[nodiscard]] bool quiesce_until(std::chrono::steady_clock::time_point deadline) noexcept;
  /// The exception text of the first executor thread that failed (a callback
  /// throw or an rclcpp/rcl failure). Both executor threads of the owner stop.
  /// Empty while healthy.
  std::optional<std::string> executor_fault() const;
  /// Monotonic failure-start boundary; may precede publication of fault text.
  bool executor_failure_started() const noexcept {
    return executor_failure_started_.load(std::memory_order_acquire);
  }
  /// The owner's dispatch has closed (quiesce began or the executor failed).
  bool dispatch_closed() const noexcept {
    return sdk_send_admission_closed_.load(std::memory_order_acquire);
  }
  /// Notifies waiters that this spinning owner began failing. It is a wake-up,
  /// not evidence that any outstanding effect has settled. Stop callbacks run
  /// on the failing executor thread (owner or time-critical) and must not
  /// throw, block or quiesce this owner.
  std::stop_token executor_failure_token() const noexcept {
    return executor_failure_source_.get_token();
  }

  /// The lifecycle authority pins and serializes the exact old slot without
  /// holding locks needed by callbacks or its fault observer. Release drops
  /// its old entity references after join, preserving outcome records and
  /// existing borrows. Factory returns a fresh shared-owned resource.
  /// Prepare stages normal entities and starts the replacement without
  /// publishing or enabling them;
  /// the caller owns cleanup of that staging on failure and commits it only
  /// with the returned owner. Any failure leaves the old owner faulted.
  /// Callers must settle or retain all non-ROS obligations (including scene
  /// projection and scheduled work) before quiesce; this helper does not
  /// cancel, drain or settle those obligations on the caller's behalf.
  template <class Release, class Factory, class Prepare>
  static auto rebuild_faulted(Ros2TransportResources &failed,
                              std::chrono::steady_clock::time_point deadline,
                              Release &&release, Factory &&factory, Prepare &&prepare)
      -> std::invoke_result_t<Factory> {
    failed.check_rebuild_allowed();
    if (!failed.quiesce_until(deadline))
      throw core::TypedError("ros_transport_quiesce_timeout",
                             "ROS transport did not quiesce before the deadline");
    std::invoke(std::forward<Release>(release));
    auto replacement = std::invoke(std::forward<Factory>(factory));
    if (!replacement || replacement.get() == &failed)
      throw std::logic_error("ROS transport rebuild requires a new owner");
    std::invoke(std::forward<Prepare>(prepare), *replacement);
    replacement->check_replacement_healthy();
    return replacement;
  }

private:
  Ros2TransportResources(std::string node_name, std::string node_namespace,
                         std::size_t executor_thread_count, ContextProvider context_provider);
  Ros2TransportResources(std::string node_name, std::string node_namespace,
                         std::size_t executor_thread_count,
                         std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
                         std::string arm_group_id, core::IClock &clock,
                         ContextProvider context_provider);
  [[nodiscard]] bool quiesce_impl(
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) noexcept;
  struct DispatchTask {
    std::function<void()> work;
    bool done = false;
    bool ran = false;
    std::exception_ptr error;
    std::source_location site{};
  };
  bool record_owner_stall_locked();
  void check_rebuild_allowed();
  void check_replacement_healthy();
  bool record_executor_fault(const char *exception_text) noexcept;
  void close_dispatch() noexcept;
  [[nodiscard]] bool close_dispatch_until(
      std::chrono::steady_clock::time_point deadline) noexcept;
  void execute_dispatch();
  bool trigger_dispatch() noexcept;
  void invoke_impl(std::function<void()> work, DispatchOrder order,
                   std::source_location site, bool creation);
  std::shared_ptr<RetirementRecord>
  make_retirement_record(std::shared_ptr<void> entity,
                         rclcpp::CallbackGroup::SharedPtr time_critical_group);
  static void release_retirement(const std::shared_ptr<RetirementState> &state,
                                 std::shared_ptr<RetirementRecord> record) noexcept;
  struct RetirementNotifier;
  static void destroy_chain(std::shared_ptr<RetirementRecord> pending) noexcept;
  /// Owner thread: destroys ordinary records and fences time-critical ones.
  /// After both threads joined (`executors_joined`) it destroys every record.
  void drain_retirements(bool executors_joined = false) noexcept;
  void execute_time_critical_fence();
  void run_time_critical_thread() noexcept;
  [[nodiscard]] bool stop_time_critical_thread(
      std::optional<std::chrono::steady_clock::time_point> deadline) noexcept;

  friend class JtcExecutionPortTestPeer;
  friend class Ros2TransportOwnerTestPeer;
  struct ContextWatch;
  std::shared_ptr<rclcpp::Context> context_;
  std::unique_ptr<ContextWatch> context_watch_;
  std::shared_ptr<RetirementState> retirement_state_;
  bool owns_private_context_ = true;
  rclcpp::Node::SharedPtr node_;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::unique_ptr<detail::SceneProjectionWorker> scene_projection_worker_;
  std::shared_ptr<rclcpp::GuardCondition> dispatch_guard_;
  std::shared_ptr<std::function<void()>> dispatch_trigger_;
  std::shared_ptr<rclcpp::executors::ExecutorNotifyWaitable> dispatch_waitable_;
  std::mutex dispatch_mutex_;
  std::atomic<bool> sdk_send_admission_closed_{false};
  mutable std::shared_timed_mutex sdk_send_mutex_;
  bool sdk_send_closed_ = false;
  std::condition_variable dispatch_cv_;
  DispatchTask *ordinary_slot_ = nullptr;
  DispatchTask *close_slot_ = nullptr;
  DispatchTask *active_task_ = nullptr;
  std::function<void()> owner_notification_;
  bool owner_notification_pending_ = false;
  bool owner_notification_active_ = false;
  std::chrono::nanoseconds owner_wait_bound_{std::chrono::seconds(4)};
  std::shared_ptr<OwnerStallSnapshot> owner_stall_snapshot_;
  std::atomic<bool> owner_stall_published_{false};
  std::uint64_t next_fifo_ticket_ = 0;
  std::uint64_t serving_fifo_ticket_ = 0;
  std::uint64_t next_close_ticket_ = 0;
  std::uint64_t serving_close_ticket_ = 0;
  std::size_t notifiers_in_flight_ = 0;
  std::size_t foreign_invocations_ = 0;
  std::unordered_set<std::thread::id> prestart_inline_threads_;
  std::size_t active_dispatches_ = 0;
  bool dispatch_stopping_ = false;
  bool dispatch_starting_ = false;
  bool owner_exited_ = false;
  std::optional<std::string> notifier_fault_;
  std::thread::id owner_thread_id_;
  std::atomic<bool> started_{false};
  std::atomic<bool> spin_finished_{false};
  std::atomic<bool> executor_failure_started_{false};
  std::stop_source executor_failure_source_;
  mutable std::mutex lifecycle_mutex_;
  std::condition_variable quiesce_cv_;
  bool quiesced_ = false;
  bool starting_ = false;
  bool quiesce_active_ = false;
  mutable std::mutex executor_fault_mutex_;
  std::optional<std::string> executor_fault_;
  ExecutorFaultObserver executor_fault_observer_;
  std::thread spin_thread_;
  // OD-66 time-critical executor: present only when enabled before start.
  // The fence group holds the fence waitable and is never removed.
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> time_critical_executor_;
  rclcpp::CallbackGroup::SharedPtr time_critical_fence_group_;
  std::shared_ptr<rclcpp::GuardCondition> time_critical_fence_guard_;
  std::shared_ptr<rclcpp::executors::ExecutorNotifyWaitable> time_critical_fence_waitable_;
  // Removal fence: requested by the owner thread after removing groups,
  // armed and then acknowledged by the time-critical thread one wait later.
  std::atomic<std::uint64_t> time_critical_fence_requested_{0};
  std::uint64_t time_critical_fence_armed_ = 0;  // time-critical thread only
  std::atomic<std::uint64_t> time_critical_fence_acked_{0};
  std::thread::id time_critical_thread_id_;      // guarded by dispatch_mutex_
  std::atomic<bool> time_critical_spin_finished_{false};
  std::thread time_critical_thread_;
};
} // namespace manyforge::ros2_control_execution
