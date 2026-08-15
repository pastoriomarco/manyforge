#include "ros2_control_stop_qualification_fixture.hpp"
#include "stop_qualification_evidence_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifndef MANYFORGE_ROS2_CONTROL_STOP_QUALIFICATION_REPETITIONS
#error "ROS Stop qualification repetition count is build-owned"
#endif
#ifndef MANYFORGE_ROS2_CONTROL_STOP_ISSUE_BOUND_NS
#error "ROS Stop issue bound is build-owned"
#endif

namespace manyforge::ros2_control_execution {
namespace {

using namespace std::chrono_literals;
namespace deployment_detail = manyforge::deployment::detail;
namespace fixture = qualification_fixture;

constexpr std::uint64_t kIssueBoundNs =
    MANYFORGE_ROS2_CONTROL_STOP_ISSUE_BOUND_NS;
constexpr int kRepetitions =
    MANYFORGE_ROS2_CONTROL_STOP_QUALIFICATION_REPETITIONS;

static_assert(kIssueBoundNs > 0U);
static_assert(kRepetitions > 0 && kRepetitions <= 256);

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

class QualificationProbe final
    : public detail::IRos2ControlStopQualificationProbe {
public:
  QualificationProbe() = default;

  explicit QualificationProbe(detail::Ros2ControlStopQualificationEvent blocked)
      : blocked_(blocked), block_enabled_(true) {}

  void observe(detail::Ros2ControlStopQualificationEvent event,
               std::string_view detail_text) override {
    std::unique_lock<std::mutex> lock(mutex_);
    events_.push_back({event, std::string(detail_text)});
    if (event == detail::Ros2ControlStopQualificationEvent::
                     kSoftwareHardIssueAccepted &&
        issue_started_at_.has_value()) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() -
                               *issue_started_at_)
                               .count();
      issue_latency_ns_ = static_cast<std::uint64_t>(
          std::max<std::int64_t>(0, elapsed));
    }
    cv_.notify_all();
    if (block_enabled_ && blocked_ == event) {
      cv_.wait(lock, [this]() { return released_; });
    }
  }

  [[nodiscard]] bool wait_for(
      detail::Ros2ControlStopQualificationEvent event,
      std::chrono::milliseconds timeout = 2s) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, event]() {
      return std::any_of(events_.begin(), events_.end(),
                         [event](const auto &row) {
                           return row.first == event;
                         });
    });
  }

  void arm_issue_observation() {
    std::lock_guard<std::mutex> lock(mutex_);
    issue_started_at_ = std::chrono::steady_clock::now();
    issue_latency_ns_.reset();
  }

  [[nodiscard]] std::optional<std::uint64_t>
  wait_for_issue(std::chrono::milliseconds timeout = 2s) const {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout,
                      [this]() { return issue_latency_ns_.has_value(); })) {
      return std::nullopt;
    }
    return issue_latency_ns_;
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
      block_enabled_ = false;
    }
    cv_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  detail::Ros2ControlStopQualificationEvent blocked_{
      detail::Ros2ControlStopQualificationEvent::kNominalProviderEntry};
  std::vector<
      std::pair<detail::Ros2ControlStopQualificationEvent, std::string>>
      events_;
  std::optional<std::chrono::steady_clock::time_point> issue_started_at_;
  std::optional<std::uint64_t> issue_latency_ns_;
  bool released_{};
  bool block_enabled_{};
};

class ObstructionRelease final {
public:
  ObstructionRelease(
      QualificationProbe &probe,
      std::optional<std::unique_lock<std::mutex>> &adapter_lock)
      : probe_(probe), adapter_lock_(adapter_lock) {}

  ObstructionRelease(const ObstructionRelease &) = delete;
  ObstructionRelease &operator=(const ObstructionRelease &) = delete;

  ~ObstructionRelease() { release(); }

  void release() noexcept {
    if (released_) {
      return;
    }
    probe_.release();
    if (adapter_lock_.has_value() && adapter_lock_->owns_lock()) {
      adapter_lock_->unlock();
    }
    released_ = true;
  }

private:
  QualificationProbe &probe_;
  std::optional<std::unique_lock<std::mutex>> &adapter_lock_;
  bool released_{};
};

enum class ObstructionCase {
  kProviderCall,
  kAdapterLock,
  kResultWait,
};

[[nodiscard]] std::string_view obstruction_name(ObstructionCase obstruction) {
  switch (obstruction) {
  case ObstructionCase::kProviderCall:
    return "nominal_provider_call_blocked";
  case ObstructionCase::kAdapterLock:
    return "nominal_adapter_lock_blocked";
  case ObstructionCase::kResultWait:
    return "nominal_result_wait_blocked";
  }
  return "unknown";
}

[[nodiscard]] bool wait_until_executing(
    Ros2ControlExecutionBackend &backend) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (backend.get_execution_snapshot().state ==
        core::ExecutionState::kExecuting) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

[[nodiscard]] bool wait_for_action_feedback(
    Ros2ControlExecutionBackend &backend) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (backend.controller_progress_metrics().action_feedback_callbacks_total >
        0U) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

[[nodiscard]] bool wait_for_managed_replacement(
    Ros2ControlExecutionBackend &backend) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (detail::Ros2ControlStopQualificationAccess::
            managed_replacement_owns_goal(backend)) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

[[nodiscard]] bool finish_cancelled_goal(fixture::FakeFjtServer &server) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (server.cancel_current_goal()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

struct ScheduledStage final {
  std::future<core::ParticipantStopStageResultDTO> result;
};

[[nodiscard]] ScheduledStage schedule_software_hard_stage(
    Ros2ControlExecutionBackend &backend,
    roboplan_backend::SimpleScheduler &scheduler,
    roboplan_backend::WallClock &clock,
    const core::ResolvedParticipantStopStageDTO &stage,
    QualificationProbe &probe) {
  auto promise =
      std::make_shared<std::promise<core::ParticipantStopStageResultDTO>>();
  auto future = promise->get_future();
  probe.arm_issue_observation();
  scheduler.schedule(
      clock.now(),
      [&backend, &stage, promise]() {
        try {
          promise->set_value(backend.TranslateStopStage(
              stage, fixture::remaining_budget(stage), std::stop_token{}));
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
      },
      core::SchedulerPriority::StopControl);
  return {std::move(future)};
}

void require_successful_stage(
    const core::ParticipantStopStageResultDTO &result,
    const ObstructionCase obstruction, const int repetition) {
  if (result.attempt_disposition !=
          core::StopStageAttemptDispositionDTO::kDispatched ||
      result.outcome != core::ParticipantStopStageOutcomeDTO::kSucceeded ||
      result.evidence_items.size() != 1U ||
      result.evidence_items.front().state !=
          core::StopEvidenceItemStateDTO::kSatisfied ||
      result.evidence_items.front().required_class !=
          core::StopEvidenceRequirementKind::kMotionRest ||
      !result.source_epoch.has_value() || !result.source_sequence.has_value()) {
    const auto reason =
        result.error.has_value() && result.error->details.contains("reason")
            ? result.error->details.at("reason")
            : "stage_result_incomplete";
    const auto detail = [&](std::string_view key) {
      if (!result.error.has_value()) {
        return std::string("unavailable");
      }
      const auto found = result.error->details.find(std::string(key));
      return found == result.error->details.end()
                 ? std::string("unavailable")
                 : found->second;
    };
    fail("software-hard stage did not retain terminal motion-rest evidence: " +
         reason + " obstruction=" + std::string(obstruction_name(obstruction)) +
         " repetition=" + std::to_string(repetition) +
         " expected_terminal_state=" + detail("expected_terminal_state") +
         " observed_terminal_state=" + detail("observed_terminal_state") +
         " terminal_sequence=" + detail("terminal_sequence"));
  }
}

[[nodiscard]] std::uint64_t run_obstruction_case(
    ObstructionCase obstruction,
    const core::ProductContractRevision &product_revision) {
  std::uint64_t maximum_latency_ns = 0U;
  for (int repetition = 0; repetition < kRepetitions; ++repetition) {
    roboplan_backend::WallClock clock;
    roboplan_backend::SimpleScheduler scheduler;
    auto binding = fixture::make_binding();
    auto stage = fixture::software_hard_stage(binding, product_revision);
    QualificationProbe probe =
        obstruction == ObstructionCase::kProviderCall
            ? QualificationProbe(detail::Ros2ControlStopQualificationEvent::
                                     kNominalProviderEntry)
        : obstruction == ObstructionCase::kResultWait
            ? QualificationProbe(detail::Ros2ControlStopQualificationEvent::
                                     kNominalResultWaitEnter)
            : QualificationProbe();
    const std::string action_name =
        "/manyforge_ros_stop_qualification_fjt_" + fixture::unique_suffix();
    fixture::FakeFjtServer server(action_name);
    auto sink = std::make_shared<fixture::StateSink>();
    auto backend = std::make_unique<Ros2ControlExecutionBackend>(
        sink, "arm", clock, scheduler,
        fixture::backend_config(binding, action_name));
    detail::Ros2ControlStopQualificationAccess::install_probe(*backend, probe);

    auto execution = backend->execute_trajectory(fixture::trajectory());
    if (!server.wait_for_goal_count(1U) || !wait_until_executing(*backend)) {
      probe.release();
      fail("ROS Stop qualification execution did not enter Executing");
    }
    if (!server.publish_feedback() || !wait_for_action_feedback(*backend)) {
      probe.release();
      fail("ROS Stop qualification could not publish active-goal feedback");
    }
    detail::Ros2ControlStopQualificationAccess::seed_observed_joint_sample(
        *backend, {"source_joint_1"}, {0.0}, {0.0});
    detail::Ros2ControlStopQualificationAccess::
        install_prepared_goal_membership(
            *backend, stage, core::ContentDigest("prepared.ros_qualification"));

    std::optional<std::unique_lock<std::mutex>> nominal_adapter_lock;
    if (obstruction == ObstructionCase::kAdapterLock) {
      nominal_adapter_lock.emplace(
          detail::Ros2ControlStopQualificationAccess::
              lock_nominal_state_adapter(*backend));
    }
    std::stop_source nominal_stop;
    auto nominal = std::async(std::launch::async, [&]() {
      return backend->execute_controlled_stop(fixture::spring_profile(),
                                              nominal_stop.get_token());
    });
    ObstructionRelease release(probe, nominal_adapter_lock);

    const auto expected_entry =
        obstruction == ObstructionCase::kProviderCall
            ? detail::Ros2ControlStopQualificationEvent::kNominalProviderEntry
        : obstruction == ObstructionCase::kAdapterLock
            ? detail::Ros2ControlStopQualificationEvent::
                  kNominalAdapterLockAttempt
            : detail::Ros2ControlStopQualificationEvent::
                  kNominalResultWaitEnter;
    if (!probe.wait_for(expected_entry)) {
      release.release();
      if (nominal.wait_for(100ms) == std::future_status::ready) {
        const auto early = nominal.get();
        const auto reason =
            early.error.has_value() && early.error->details.contains("reason")
                ? early.error->details.at("reason")
                : "no_reason";
        fail("nominal ROS Stop did not reach " +
             std::string(obstruction_name(obstruction)) + " in repetition " +
             std::to_string(repetition) + ": provider returned " + reason);
      }
      fail("nominal ROS Stop stalled before " +
           std::string(obstruction_name(obstruction)) + " in repetition " +
           std::to_string(repetition));
    }
    if (obstruction == ObstructionCase::kAdapterLock &&
        nominal.wait_for(5ms) == std::future_status::ready) {
      fail("nominal ROS Stop did not block on its real state-adapter lock");
    }
    if (obstruction == ObstructionCase::kResultWait &&
        (!server.wait_for_goal_count(2U) ||
         !wait_for_managed_replacement(*backend))) {
      fail("nominal ROS Stop did not park on its managed result wait");
    }

    nominal_stop.request_stop();
    auto scheduled = schedule_software_hard_stage(
        *backend, scheduler, clock, stage, probe);
    const auto issue_latency = probe.wait_for_issue();
    if (!issue_latency.has_value()) {
      release.release();
      if (scheduled.result.wait_for(10ms) == std::future_status::ready) {
        const auto early = scheduled.result.get();
        const auto reason =
            early.error.has_value() && early.error->details.contains("reason")
                ? early.error->details.at("reason")
                : "no_reason";
        const auto detail = [&](std::string_view key) {
          if (!early.error.has_value()) {
            return std::string("unavailable");
          }
          const auto found = early.error->details.find(std::string(key));
          return found == early.error->details.end()
                     ? std::string("unavailable")
                     : found->second;
        };
        fail("software-hard ROS StopControl issue point was not reached in " +
             std::string(obstruction_name(obstruction)) + " repetition " +
             std::to_string(repetition) + ": stage returned " + reason +
             " prepared_generation=" +
             detail("prepared_execution_generation") +
             " current_generation=" +
             detail("current_execution_generation") + " activity=" +
             detail("current_execution_activity"));
      }
      fail("software-hard ROS StopControl issue point stalled in " +
           std::string(obstruction_name(obstruction)) + " repetition " +
           std::to_string(repetition));
    }
    if (*issue_latency > kIssueBoundNs) {
      fail("software-hard ROS production issue exceeded its qualified bound");
    }
    maximum_latency_ns = std::max(maximum_latency_ns, *issue_latency);

    if (!server.wait_for_cancel_requests(1U) ||
        !finish_cancelled_goal(server)) {
      fail("software-hard ROS Stop did not cancel the active exact JTC goal");
    }
    release.release();

    if (scheduled.result.wait_for(2s) != std::future_status::ready) {
      fail("software-hard ROS Stop stage did not finish its bounded evidence");
    }
    require_successful_stage(scheduled.result.get(), obstruction, repetition);
    if (nominal.wait_for(2s) != std::future_status::ready) {
      fail("nominal ROS Stop did not unwind after software-hard escalation");
    }
    static_cast<void>(nominal.get());
    const std::uint64_t expected_goals =
        obstruction == ObstructionCase::kResultWait ? 2U : 1U;
    if (server.goal_count() != expected_goals) {
      fail("cancelled nominal ROS Stop dispatched after hard escalation");
    }
    if (!execution.wait_for_completion(2'000'000'000LL) ||
        backend->get_execution_snapshot().state !=
            core::ExecutionState::kStopped) {
      fail("software-hard ROS Stop did not commit the exact execution terminal");
    }
    if (!scheduler.drain_for(core::SchedulerPriority::StopControl, 2s)) {
      fail("ROS StopControl scheduler did not drain after qualification");
    }
  }
  return maximum_latency_ns;
}

[[nodiscard]] std::string read_file(const std::filesystem::path &path,
                                    std::string_view label) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    fail("cannot open " + std::string(label));
  }
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string canonical_root_text(
    std::string_view bytes, std::string_view field, std::string_view label) {
  deployment::CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception &error) {
    fail(std::string(label) + " is invalid JSON: " + error.what());
  }
  if (deployment::canonical_json(root) != bytes) {
    fail(std::string(label) + " is not canonical JSON");
  }
  const auto *member = root.find_object_member(field);
  if (member == nullptr) {
    fail(std::string(label) + " is missing " + std::string(field));
  }
  const auto *text = std::get_if<std::string>(&member->storage());
  if (text == nullptr || text->empty()) {
    fail(std::string(label) + " has a non-text " + std::string(field));
  }
  return *text;
}

void write_atomic(const std::filesystem::path &path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream ||
        !stream.write(bytes.data(),
                      static_cast<std::streamsize>(bytes.size())) ||
        !stream.flush()) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      fail("could not write ROS Stop qualification output");
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    fail("could not atomically publish ROS Stop qualification output");
  }
}

struct Arguments final {
  std::filesystem::path implementation_build;
  std::filesystem::path runner_contract;
  std::filesystem::path implementation_output;
};

[[nodiscard]] Arguments parse_arguments(int argc, char **argv) {
  if (argc != 7) {
    fail("usage: runner --implementation-build PATH --runner-contract PATH "
         "--implementation-output PATH");
  }
  std::map<std::string, std::filesystem::path, std::less<>> values;
  for (int index = 1; index < argc; index += 2) {
    if (argv[index] == nullptr || argv[index + 1] == nullptr ||
        !values.emplace(argv[index], argv[index + 1]).second) {
      fail("runner arguments are malformed or duplicated");
    }
  }
  const auto require = [&](std::string_view name) {
    const auto found = values.find(name);
    if (found == values.end() || found->second.empty()) {
      fail("runner is missing " + std::string(name));
    }
    return found->second;
  };
  return {require("--implementation-build"), require("--runner-contract"),
          require("--implementation-output")};
}

int run(int argc, char **argv) {
  const auto arguments = parse_arguments(argc, argv);
  const auto implementation_build_bytes =
      read_file(arguments.implementation_build, "implementation build closure");
  const auto runner_contract_bytes =
      read_file(arguments.runner_contract, "runner contract");
  const auto implementation_id = canonical_root_text(
      implementation_build_bytes, "implementation_id",
      "implementation build closure");
  if (implementation_id !=
      deployment::ros2_control_execution_runtime_contract::kImplementationId) {
    fail("implementation build closure does not name the ROS execution host");
  }
  const auto implementation_fingerprint = canonical_root_text(
      implementation_build_bytes, "implementation_source_build_fingerprint",
      "implementation build closure");
  const auto runner_digest = canonical_root_text(
      runner_contract_bytes, "qualification_runner_contract_digest",
      "runner contract");

  const auto product_revision = deployment_detail::
      ros2_control_execution_runtime_product_contract_revision();
  const auto provider_max =
      run_obstruction_case(ObstructionCase::kProviderCall, product_revision);
  const auto lock_max =
      run_obstruction_case(ObstructionCase::kAdapterLock, product_revision);
  const auto wait_max =
      run_obstruction_case(ObstructionCase::kResultWait, product_revision);

  const core::ProviderContractRefDTO hard_provider{
      core::ProviderContractRoleId(std::string(
          detail::kRos2ControlSoftwareHardProviderContractRoleId)),
      core::SchemaId(
          std::string(detail::kRos2ControlSoftwareHardContractSchemaId)),
      core::SchemaRevision(
          std::string(detail::kRos2ControlSoftwareHardContractSchemaRevision))};
  const deployment_detail::BuildQualifiedStopImplementationEvidenceEntry entry{
      implementation_id,
      core::ContentDigest(implementation_fingerprint),
      hard_provider,
      core::ProductContractId(std::string(
          deployment::ros2_control_execution_runtime_contract::
              kProductContractId)),
      product_revision,
      core::RegisteredStopCommandKindId("manyforge.stop.software_hard"),
      std::nullopt,
      core::StopInvocationLaneId(std::string(
          detail::kRos2ControlSoftwareHardStopInvocationLaneId)),
      kIssueBoundNs,
      {{{deployment_detail::StopQualificationFaultCase::
             kNominalProviderCallBlocked,
         provider_max},
        {deployment_detail::StopQualificationFaultCase::
             kNominalAdapterLockBlocked,
         lock_max},
        {deployment_detail::StopQualificationFaultCase::
             kNominalResultWaitBlocked,
         wait_max}}}};
  const auto package =
      deployment_detail::emit_stop_implementation_qualification_evidence_package(
          core::ContentDigest(runner_digest),
          std::span<const deployment_detail::
                        BuildQualifiedStopImplementationEvidenceEntry>(&entry,
                                                                       1U));
  write_atomic(arguments.implementation_output, package);
  return 0;
}

} // namespace
} // namespace manyforge::ros2_control_execution

int main(int argc, char **argv) {
  try {
    return manyforge::ros2_control_execution::run(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << "ROS Stop qualification failed: " << error.what()
              << std::endl;
    return 1;
  }
}
