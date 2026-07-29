// Copyright 2026 Flexin Group SRL
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

#include <manyforge_xarm_control/lifecycle_ros_port.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <manyforge_xarm_control/lite6_lifecycle_catalog.hpp>
#include <manyforge_xarm_control/real_hardware_backend.hpp>

namespace manyforge::xarm_control
{
namespace
{

std::int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

/// Single-owner NRT command queue around the production hardware backend.
///
/// Observation, admission-gate, and controller-activity operations delegate
/// directly to the concurrency-safe backend. Only lifecycle mutations are
/// serialized on the dedicated worker so bounded ROS waits cannot starve
/// provider health, status publication, or authority renewal.
class SerializedXArmHardwareBackend final : public IXArmHardwareBackend
{
public:
  struct Work
  {
    XArmLifecycleCommand command = XArmLifecycleCommand::kClearError;
    std::string request_id;
    std::uint64_t expected_generation = 0;
  };

  struct GateWork
  {
    std::uint64_t sequence = 0;
    bool open = false;
    std::int64_t valid_until_ns = 0;
  };

  explicit SerializedXArmHardwareBackend(
    std::unique_ptr<IXArmHardwareBackend> backend)
  : backend_(std::move(backend))
  {
    if (backend_ == nullptr) {
      throw std::invalid_argument(
              "serialized xArm hardware backend is missing");
    }
  }

  [[nodiscard]] std::string_view owner_id() const noexcept override
  {
    return backend_->owner_id();
  }

  [[nodiscard]] XArmObservedState observe(
    std::int64_t now_ns) const override
  {
    return backend_->observe(now_ns);
  }

  XArmLifecycleCommandResult execute_lifecycle(
    XArmLifecycleCommand command,
    std::string_view request_id,
    std::uint64_t expected_generation) override
  {
    XArmLifecycleCommandResult result;
    result.observed_generation = expected_generation;
    {
      std::scoped_lock lock(work_mutex_);
      if (stopping_.load(std::memory_order_acquire)) {
        result.reason = "xarm_lifecycle_worker_stopping";
        return result;
      }
      if (queued_work_.has_value() || work_in_flight_) {
        result.reason = "xarm_lifecycle_worker_busy";
        return result;
      }
      queued_work_ = Work{
        command, std::string(request_id), expected_generation};
      result.state = XArmLifecycleResultState::kExecutionDeferred;
      result.reason = "xarm_lifecycle_worker_accepted";
    }
    work_cv_.notify_one();
    return result;
  }

  bool read_joint_state(JointStateSample & sample) override
  {
    return backend_->read_joint_state(sample);
  }

  bool write_joint_command(const JointCommandView & command) override
  {
    return backend_->write_joint_command(command);
  }

  XArmCommandGateUpdate set_command_gate(
    bool open, std::int64_t valid_until_ns) override
  {
    const std::int64_t now_ns = steady_now_ns();
    if (open && valid_until_ns <= now_ns) {
      return {};
    }
    XArmCommandGateUpdate current;
    {
      std::scoped_lock lock(gate_mutex_);
      if (stopping_.load(std::memory_order_acquire)) {
        return current;
      }
      const std::int64_t normalized_valid_until_ns =
        open ? valid_until_ns : 0;
      const bool desired_changed =
        desired_gate_open_ != open ||
        (open &&
        normalized_valid_until_ns != desired_gate_valid_until_ns_);
      const bool desired_not_applied =
        !applied_gate_known_ ||
        applied_gate_open_ != desired_gate_open_ ||
        (desired_gate_open_ &&
        applied_gate_valid_until_ns_ != desired_gate_valid_until_ns_);
      const bool needs_gate_dispatch =
        !queued_gate_work_.has_value() &&
        !gate_work_in_flight_.has_value() &&
        desired_not_applied;
      if (desired_changed) {
        desired_gate_open_ = open;
        desired_gate_valid_until_ns_ = normalized_valid_until_ns;
        queued_gate_work_ = GateWork{
          ++next_gate_sequence_, open, normalized_valid_until_ns};
      } else if (needs_gate_dispatch) {
        queued_gate_work_ = GateWork{
          ++next_gate_sequence_, desired_gate_open_,
          desired_gate_valid_until_ns_};
      }

      const bool conflicting_in_flight =
        gate_work_in_flight_.has_value() &&
        gate_work_in_flight_->open != desired_gate_open_;
      const bool closed_gate_applied =
        applied_gate_known_ && !applied_gate_open_ &&
        !gate_work_in_flight_.has_value() &&
        !queued_gate_work_.has_value();
      if (desired_gate_open_) {
        if (applied_gate_known_ && applied_gate_open_ &&
          applied_gate_valid_until_ns_ > now_ns &&
          !conflicting_in_flight)
        {
          current.state = XArmCommandGateUpdateState::kApplied;
          current.effective_valid_until_ns =
            applied_gate_valid_until_ns_;
        } else {
          current.state = XArmCommandGateUpdateState::kPending;
        }
      } else if (closed_gate_applied) {
        current.state = XArmCommandGateUpdateState::kApplied;
      } else {
        current.state = XArmCommandGateUpdateState::kPending;
      }
    }
    gate_cv_.notify_one();
    return current;
  }

  void set_hardware_component_active(bool active) override
  {
    backend_->set_hardware_component_active(active);
  }

  void set_trajectory_controller_active(bool active) override
  {
    backend_->set_trajectory_controller_active(active);
  }

  [[nodiscard]] std::optional<Work> wait_for_work()
  {
    std::unique_lock lock(work_mutex_);
    work_cv_.wait(
      lock, [this]() {
        return stopping_.load(std::memory_order_acquire) ||
               queued_work_.has_value();
      });
    if (stopping_.load(std::memory_order_acquire)) {
      queued_work_.reset();
      return std::nullopt;
    }
    Work work = std::move(*queued_work_);
    queued_work_.reset();
    work_in_flight_ = true;
    return work;
  }

  XArmLifecycleCommandResult execute_work(const Work & work)
  {
    return backend_->execute_lifecycle(
      work.command, work.request_id, work.expected_generation);
  }

  void finish_work()
  {
    std::scoped_lock lock(work_mutex_);
    work_in_flight_ = false;
  }

  void request_stop()
  {
    {
      std::scoped_lock lock(work_mutex_);
      stopping_.store(true, std::memory_order_release);
      queued_work_.reset();
    }
    work_cv_.notify_all();
    {
      std::scoped_lock lock(gate_mutex_);
      queued_gate_work_.reset();
    }
    gate_cv_.notify_all();
  }

  [[nodiscard]] bool stopping() const
  {
    return stopping_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::optional<GateWork> wait_for_gate_work()
  {
    std::unique_lock lock(gate_mutex_);
    gate_cv_.wait(
      lock, [this]() {
        return stopping_.load(std::memory_order_acquire) ||
               queued_gate_work_.has_value();
      });
    if (stopping_.load(std::memory_order_acquire)) {
      queued_gate_work_.reset();
      return std::nullopt;
    }
    GateWork work = std::move(*queued_gate_work_);
    queued_gate_work_.reset();
    gate_work_in_flight_ = work;
    return work;
  }

  XArmCommandGateUpdate execute_gate_work(const GateWork & work)
  {
    return backend_->set_command_gate(work.open, work.valid_until_ns);
  }

  void finish_gate_work(
    const GateWork & work,
    const XArmCommandGateUpdate & result)
  {
    {
      std::scoped_lock lock(gate_mutex_);
      if (gate_work_in_flight_.has_value() &&
        gate_work_in_flight_->sequence == work.sequence)
      {
        gate_work_in_flight_.reset();
      }
      if (result.state == XArmCommandGateUpdateState::kApplied) {
        applied_gate_known_ = true;
        applied_gate_open_ = work.open;
        applied_gate_valid_until_ns_ =
          work.open ? result.effective_valid_until_ns : 0;
      }
      const bool desired_applied =
        applied_gate_known_ &&
        applied_gate_open_ == desired_gate_open_ &&
        (!desired_gate_open_ ||
        applied_gate_valid_until_ns_ == desired_gate_valid_until_ns_);
      const bool desired_changed_while_in_flight =
        desired_gate_open_ != work.open ||
        desired_gate_valid_until_ns_ != work.valid_until_ns;
      if (!stopping_.load(std::memory_order_acquire) &&
        !desired_applied &&
        !queued_gate_work_.has_value() &&
        desired_changed_while_in_flight)
      {
        queued_gate_work_ = GateWork{
          ++next_gate_sequence_, desired_gate_open_,
          desired_gate_valid_until_ns_};
      }
    }
    gate_cv_.notify_one();
  }

private:
  std::unique_ptr<IXArmHardwareBackend> backend_;
  mutable std::mutex work_mutex_;
  std::condition_variable work_cv_;
  std::optional<Work> queued_work_;
  bool work_in_flight_ = false;
  mutable std::mutex gate_mutex_;
  std::condition_variable gate_cv_;
  std::optional<GateWork> queued_gate_work_;
  std::optional<GateWork> gate_work_in_flight_;
  std::uint64_t next_gate_sequence_ = 0;
  bool desired_gate_open_ = false;
  std::int64_t desired_gate_valid_until_ns_ = 0;
  bool applied_gate_known_ = false;
  bool applied_gate_open_ = false;
  std::int64_t applied_gate_valid_until_ns_ = 0;
  std::atomic<bool> stopping_{false};
};

XArmLifecycleRosPort::XArmLifecycleRosPort(
  XArmLifecycleRosPortConfig config)
{
  if (config.robot_id.empty() ||
    config.supervisor_instance_id.empty() ||
    config.observation_lease_ns <= 0)
  {
    throw std::invalid_argument(
            "xArm lifecycle ROS port configuration is incomplete");
  }
  const std::string owner_id = config.transport.owner_id;
  auto transport =
    std::make_unique<XArmRosTransportSession>(
    std::move(config.transport));
  RealXArmHardwareBackendConfig backend_config;
  backend_config.transport_owner_id = owner_id;
  auto backend = std::make_unique<RealXArmHardwareBackend>(
    std::move(backend_config), std::move(transport));
  auto serialized_backend =
    std::make_unique<SerializedXArmHardwareBackend>(
    std::move(backend));
  serialized_backend_ = serialized_backend.get();
  robot_lifecycle::SupervisorConfig supervisor_config;
  supervisor_config.robot_id = std::move(config.robot_id);
  supervisor_config.supervisor_instance_id =
    std::move(config.supervisor_instance_id);
  supervisor_config.authority_profile = config.authority_profile;
  coordinator_ = std::make_unique<XArmLifecycleCoordinator>(
    std::move(supervisor_config), std::move(serialized_backend),
    make_lite6_xarm_action_plans(), config.observation_lease_ns, 128,
    make_lite6_xarm_target_plans());
  lifecycle_worker_ =
    std::thread([this]() {lifecycle_worker();});
  command_gate_worker_ =
    std::thread([this]() {command_gate_worker();});
}

XArmLifecycleRosPort::~XArmLifecycleRosPort()
{
  serialized_backend_->request_stop();
  if (lifecycle_worker_.joinable()) {
    lifecycle_worker_.join();
  }
  if (command_gate_worker_.joinable()) {
    command_gate_worker_.join();
  }
}

void XArmLifecycleRosPort::lifecycle_worker()
{
  while (true) {
    auto work = serialized_backend_->wait_for_work();
    if (!work.has_value()) {
      return;
    }
    XArmLifecycleCommandResult result;
    try {
      result = serialized_backend_->execute_work(*work);
      if (result.state == XArmLifecycleResultState::kExecutionDeferred) {
        result = XArmLifecycleCommandResult{};
        result.state =
          XArmLifecycleResultState::kOutcomeUnknownAfterDispatch;
        result.reason = "xarm_lifecycle_worker_nested_defer";
        result.observed_generation = work->expected_generation;
      }
    } catch (const std::exception & error) {
      result.state =
        XArmLifecycleResultState::kOutcomeUnknownAfterDispatch;
      result.reason =
        std::string("xarm_lifecycle_worker_exception:") + error.what();
      result.observed_generation = work->expected_generation;
    } catch (...) {
      result.state =
        XArmLifecycleResultState::kOutcomeUnknownAfterDispatch;
      result.reason = "xarm_lifecycle_worker_unknown_exception";
      result.observed_generation = work->expected_generation;
    }
    serialized_backend_->finish_work();
    if (serialized_backend_->stopping()) {
      return;
    }
    std::scoped_lock lock(mutex_);
    static_cast<void>(coordinator_->complete_deferred_primitive(
        work->command, work->request_id, work->expected_generation,
        result, steady_now_ns()));
  }
}

void XArmLifecycleRosPort::command_gate_worker()
{
  while (true) {
    auto work = serialized_backend_->wait_for_gate_work();
    if (!work.has_value()) {
      return;
    }
    XArmCommandGateUpdate result;
    try {
      result = serialized_backend_->execute_gate_work(*work);
    } catch (...) {
      result = {};
    }
    serialized_backend_->finish_gate_work(*work, result);
  }
}

bool XArmLifecycleRosPort::establish_authority_session(
  const robot_lifecycle::AuthoritySession & session,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  return coordinator_->establish_authority_session(session, now_ns);
}

bool XArmLifecycleRosPort::clear_authority_session(
  std::string_view session_id,
  std::uint64_t session_generation,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  return coordinator_->clear_authority_session(
    session_id, session_generation, now_ns);
}

robot_lifecycle::CommandDecision
XArmLifecycleRosPort::submit_lifecycle(
  const robot_lifecycle::CommandRequest & request,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  return coordinator_->submit(request, now_ns);
}

bool XArmLifecycleRosPort::reconcile_lifecycle(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::string reason,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  return coordinator_->reconcile(
    transaction_generation, request_id, std::move(reason), now_ns);
}

bool XArmLifecycleRosPort::refresh_lifecycle(std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  return coordinator_->refresh(now_ns);
}

robot_lifecycle::LifecycleSnapshot
XArmLifecycleRosPort::lifecycle_snapshot(
  std::int64_t now_ns) const
{
  std::scoped_lock lock(mutex_);
  return coordinator_->snapshot(now_ns);
}

std::vector<XArmPrimitiveAuditRecord>
XArmLifecycleRosPort::primitive_audit_snapshot() const
{
  std::scoped_lock lock(mutex_);
  return coordinator_->primitive_audit_snapshot();
}

}  // namespace manyforge::xarm_control
