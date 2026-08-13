#include "native_stop_qualification_fixture.hpp"
#include "native_stop_qualification_probe.hpp"
#include "stop_qualification_evidence_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifndef MANYFORGE_NATIVE_STOP_QUALIFICATION_REPETITIONS
#error "native Stop qualification repetition count is build-owned"
#endif
#ifndef MANYFORGE_NATIVE_STOP_ISSUE_BOUND_NS
#error "native Stop issue bound is build-owned"
#endif
#ifndef MANYFORGE_NATIVE_STOP_NOMINAL_STAGE_BUDGET_NS
#error "native Stop nominal-stage budget is build-owned"
#endif

namespace manyforge::roboplan_backend {
namespace {

using namespace std::chrono_literals;
namespace fixture = qualification_fixture;
namespace deployment_detail = manyforge::deployment::detail;

constexpr std::uint64_t kIssueBoundNs =
    MANYFORGE_NATIVE_STOP_ISSUE_BOUND_NS;
constexpr std::uint64_t kNominalStageBudgetNs =
    MANYFORGE_NATIVE_STOP_NOMINAL_STAGE_BUDGET_NS;
constexpr int kRepetitions = MANYFORGE_NATIVE_STOP_QUALIFICATION_REPETITIONS;

static_assert(kIssueBoundNs > 0U);
static_assert(kNominalStageBudgetNs > kIssueBoundNs);
static_assert(kRepetitions > 0 && kRepetitions <= 256);

[[noreturn]] void fail(std::string message) {
  throw std::runtime_error(std::move(message));
}

class QualificationProbe final : public detail::INativeStopQualificationProbe {
 public:
  QualificationProbe() = default;

  explicit QualificationProbe(detail::NativeStopQualificationEvent blocked)
      : blocked_(blocked), block_enabled_(true) {}

  void observe(detail::NativeStopQualificationEvent event,
               std::string_view detail_text) override {
    std::unique_lock lock(mutex_);
    events_.push_back({event, std::string(detail_text)});
    cv_.notify_all();
    if (block_enabled_ && blocked_ == event) {
      cv_.wait(lock, [this] { return released_; });
    }
  }

  [[nodiscard]] bool wait_for(detail::NativeStopQualificationEvent event,
                              std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return contains_locked(event); });
  }

  [[nodiscard]] std::optional<std::size_t> first_index(
      detail::NativeStopQualificationEvent event) const {
    std::lock_guard lock(mutex_);
    for (std::size_t index = 0U; index < events_.size(); ++index) {
      if (events_[index].first == event) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::string first_detail(
      detail::NativeStopQualificationEvent event) const {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(
        events_.begin(), events_.end(),
        [event](const auto& row) { return row.first == event; });
    return found == events_.end() ? std::string{} : found->second;
  }

  void release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

 private:
  [[nodiscard]] bool contains_locked(
      detail::NativeStopQualificationEvent event) const {
    return std::any_of(events_.begin(), events_.end(),
                       [event](const auto& row) {
                         return row.first == event;
                       });
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  detail::NativeStopQualificationEvent blocked_{
      detail::NativeStopQualificationEvent::kNominalProviderEntry};
  std::vector<std::pair<detail::NativeStopQualificationEvent, std::string>>
      events_;
  bool released_{};
  bool block_enabled_{};
};

class NominalObstructionRelease final {
 public:
  NominalObstructionRelease(
      QualificationProbe& probe,
      std::optional<std::unique_lock<std::mutex>>& adapter_lock)
      : probe_(probe), adapter_lock_(adapter_lock) {}

  NominalObstructionRelease(const NominalObstructionRelease&) = delete;
  NominalObstructionRelease& operator=(const NominalObstructionRelease&) =
      delete;

  ~NominalObstructionRelease() { release(); }

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
  QualificationProbe& probe_;
  std::optional<std::unique_lock<std::mutex>>& adapter_lock_;
  bool released_{};
};

[[nodiscard]] bool wait_until_executing(RoboPlanExecutionBackend& backend,
                                        double minimum_progress = 0.0) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = backend.get_execution_snapshot();
    if (snapshot.state == core::ExecutionState::kExecuting &&
        snapshot.progress >= minimum_progress) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

struct IssueObservation final {
  core::StopResultDTO result;
  std::uint64_t latency_ns{};
};

[[nodiscard]] IssueObservation issue_on_real_stop_control_lane(
    RoboPlanExecutionBackend& backend, SimpleScheduler& scheduler) {
  auto attempt = std::make_shared<core::HardStopFloorAttempt>();
  auto promise = std::make_shared<std::promise<IssueObservation>>();
  auto future = promise->get_future();
  const auto started = std::chrono::steady_clock::now();
  const auto immediate_deadline =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          started.time_since_epoch())
          .count();
  scheduler.schedule(
      immediate_deadline,
      [&backend, attempt, started, promise]() {
        try {
          auto result = core::request_hard_stop_floor_once(backend, attempt);
          const auto latency =
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - started)
                  .count();
          promise->set_value(IssueObservation{
              std::move(result), static_cast<std::uint64_t>(
                                     std::max<std::int64_t>(0, latency))});
        } catch (...) {
          promise->set_exception(std::current_exception());
        }
      },
      core::SchedulerPriority::StopControl);
  if (future.wait_for(2s) != std::future_status::ready) {
    fail("software-hard StopControl issue lane did not return");
  }
  auto observation = future.get();
  if (!observation.result.accepted) {
    fail("software-hard production issue path rejected an active execution");
  }
  if (observation.latency_ns > kIssueBoundNs) {
    fail("software-hard production issue exceeded its qualified bound");
  }
  return observation;
}

enum class ObstructionCase {
  kProviderCall,
  kAdapterLock,
  kResultWait,
};

[[nodiscard]] std::uint64_t run_obstruction_case(ObstructionCase obstruction) {
  std::uint64_t maximum_latency_ns = 0U;
  for (int repetition = 0; repetition < kRepetitions; ++repetition) {
    WallClock clock;
    auto binding = fixture::make_binding();
    auto channel = std::make_shared<fixture::StateChannel>(binding);
    RoboPlanExecutionBackendConfig config;
    config.soft_on_path_max_jerk_rad_s3 = {
        {"joint_1", 1'000'000.0}, {"joint_2", 1'000'000.0}};
    QualificationProbe probe =
        obstruction == ObstructionCase::kProviderCall
            ? QualificationProbe(
                  detail::NativeStopQualificationEvent::kNominalProviderEntry)
        : obstruction == ObstructionCase::kResultWait
            ? QualificationProbe(detail::NativeStopQualificationEvent::
                                     kWorkerStopObservedBeforeTerminal)
            : QualificationProbe();
    auto backend = detail::NativeStopQualificationAccess::construct(
        binding, channel, clock, config, probe);
    auto execution = backend->execute_trajectory(
        fixture::trajectory(binding, /*duration_s=*/5.0));
    if (!wait_until_executing(*backend)) {
      probe.release();
      fail("qualification execution did not enter Executing");
    }

    std::optional<std::unique_lock<std::mutex>> nominal_adapter_lock;
    if (obstruction == ObstructionCase::kAdapterLock) {
      nominal_adapter_lock.emplace(
          detail::NativeStopQualificationAccess::lock_nominal_adapter(
              *backend));
    }

    auto profile =
        fixture::soft_profile(binding, /*deceleration_s=*/0.30);
    auto nominal = std::async(std::launch::async, [&] {
      return backend->execute_controlled_stop(profile);
    });
    NominalObstructionRelease release(probe, nominal_adapter_lock);

    const auto expected_entry =
        obstruction == ObstructionCase::kProviderCall
            ? detail::NativeStopQualificationEvent::kNominalProviderEntry
        : obstruction == ObstructionCase::kAdapterLock
            ? detail::NativeStopQualificationEvent::kNominalAdapterLockAttempt
            : detail::NativeStopQualificationEvent::kNominalResultWaitEnter;
    if (!probe.wait_for(expected_entry)) {
      fail("nominal Stop did not reach the selected obstruction");
    }
    if (obstruction == ObstructionCase::kResultWait &&
        !probe.wait_for(detail::NativeStopQualificationEvent::
                            kWorkerStopObservedBeforeTerminal)) {
      fail("nominal result-wait case did not park the real worker");
    }

    SimpleScheduler scheduler;
    const auto observation =
        issue_on_real_stop_control_lane(*backend, scheduler);
    maximum_latency_ns =
        std::max(maximum_latency_ns, observation.latency_ns);

    release.release();
    if (nominal.wait_for(2s) != std::future_status::ready) {
      fail("nominal Stop did not unwind after software-hard issue");
    }
    static_cast<void>(nominal.get());
    if (!execution.wait_for_completion(2'000'000'000LL)) {
      fail("qualification execution did not drain after software-hard issue");
    }
    if (!scheduler.drain_for(core::SchedulerPriority::StopControl, 1s)) {
      fail("StopControl scheduler did not drain after qualification issue");
    }
  }
  return maximum_latency_ns;
}

struct SubstitutionQualification final {
  std::array<deployment_detail::ControlledStopSubstitutionQualificationCase,
             4U>
      cases;
};

[[nodiscard]] SubstitutionQualification run_real_substitution() {
  core::FakeClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_stop_traj_delta_s = 0.02;
  config.soft_on_path_sample_period_s = 0.01;
  config.soft_on_path_max_jerk_rad_s3 = {
      {"joint_1", 1'000'000.0}, {"joint_2", 1'000'000.0}};
  QualificationProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  const auto expected_scope =
      detail::NativeStopQualificationAccess::scope(*backend);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 1.0));
  if (!wait_until_executing(*backend)) {
    fail("substitution execution did not enter Executing");
  }
  if (!probe.wait_for(
          detail::NativeStopQualificationEvent::kExecutionLoopArmed)) {
    fail("substitution execution loop did not arm");
  }
  clock.advance(950'000'000);
  if (!wait_until_executing(*backend, 0.94)) {
    fail("substitution execution did not reach the deterministic late cursor");
  }

  auto profile = fixture::soft_profile(binding, /*deceleration_s=*/0.15);
  const auto started = std::chrono::steady_clock::now();
  auto result = backend->execute_controlled_stop(profile);
  const auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started)
          .count();
  if (!result.accepted || !result.backend_stop_confirmed ||
      result.executed_stop_mode !=
          core::MotionInterruptStopMode::kSpringBackHold) {
    fail("real soft_on_path did not execute the qualified spring_back_hold substitution");
  }
  if (!result.fallback_reason.has_value() ||
      *result.fallback_reason != "soft_on_path_remaining_time_short" ||
      !result.diagnostics.contains("requested_stop_mode") ||
      core::interrupt_structured_scalar_text(
          result.diagnostics.at("requested_stop_mode")) != "soft_on_path" ||
      !result.diagnostics.contains("fallback_reason") ||
      core::interrupt_structured_scalar_text(
          result.diagnostics.at("fallback_reason")) !=
          *result.fallback_reason) {
    fail("real substitution did not retain requested/executed/reason truth");
  }
  const auto selection = probe.first_index(
      detail::NativeStopQualificationEvent::kSubstitutionSelected);
  const auto dispatch = probe.first_index(
      detail::NativeStopQualificationEvent::kPhysicalDispatch);
  if (!selection.has_value() || !dispatch.has_value() ||
      *selection >= *dispatch ||
      probe.first_detail(
          detail::NativeStopQualificationEvent::kSubstitutionSelected) !=
          "soft_on_path_remaining_time_short") {
    fail("substitution was not selected before physical dispatch");
  }
  if (detail::NativeStopQualificationAccess::scope(*backend) !=
      expected_scope) {
    fail("substitution changed the nominal component/target/resource scope");
  }
  if (elapsed_ns < 0 ||
      static_cast<std::uint64_t>(elapsed_ns) > kNominalStageBudgetNs) {
    fail("substitution exceeded the original nominal stage budget");
  }
  if (!execution.wait_for_completion(2'000'000'000LL)) {
    fail("substitution execution handle did not drain");
  }

  return {{deployment_detail::ControlledStopSubstitutionQualificationCase::
               kSelectedBeforePhysicalDispatch,
           deployment_detail::ControlledStopSubstitutionQualificationCase::
               kNominalScopePreserved,
           deployment_detail::ControlledStopSubstitutionQualificationCase::
               kNominalStageBudgetPreserved,
           deployment_detail::ControlledStopSubstitutionQualificationCase::
               kRequestedExecutedReasonRetained}};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path,
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
  } catch (const std::exception& error) {
    fail(std::string(label) + " is invalid JSON: " + error.what());
  }
  if (deployment::canonical_json(root) != bytes) {
    fail(std::string(label) + " is not canonical JSON");
  }
  const auto* member = root.find_object_member(field);
  if (member == nullptr) {
    fail(std::string(label) + " is missing " + std::string(field));
  }
  const auto* text = std::get_if<std::string>(&member->storage());
  if (text == nullptr || text->empty()) {
    fail(std::string(label) + " has a non-text " + std::string(field));
  }
  return *text;
}

void write_atomic(const std::filesystem::path& path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream || !stream.write(bytes.data(),
                                 static_cast<std::streamsize>(bytes.size())) ||
        !stream.flush()) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      fail("could not write qualification output");
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    fail("could not atomically publish qualification output");
  }
}

struct Arguments final {
  std::filesystem::path implementation_build;
  std::filesystem::path runner_contract;
  std::filesystem::path implementation_output;
  std::filesystem::path substitution_output;
};

[[nodiscard]] Arguments parse_arguments(int argc, char** argv) {
  if (argc != 9) {
    fail("usage: runner --implementation-build PATH --runner-contract PATH "
         "--implementation-output PATH --substitution-output PATH");
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
          require("--implementation-output"),
          require("--substitution-output")};
}

int run(int argc, char** argv) {
  const auto arguments = parse_arguments(argc, argv);
  const auto implementation_build_bytes =
      read_file(arguments.implementation_build, "implementation build closure");
  const auto runner_contract_bytes =
      read_file(arguments.runner_contract, "runner contract");
  const auto implementation_id = canonical_root_text(
      implementation_build_bytes, "implementation_id",
      "implementation build closure");
  if (implementation_id !=
      deployment::native_execution_runtime_contract::kImplementationId) {
    fail("implementation build closure does not name the native execution host");
  }
  const auto implementation_fingerprint = canonical_root_text(
      implementation_build_bytes, "implementation_source_build_fingerprint",
      "implementation build closure");
  const auto runner_digest = canonical_root_text(
      runner_contract_bytes, "qualification_runner_contract_digest",
      "runner contract");

  const auto provider_revision =
      deployment_detail::native_execution_runtime_product_contract_revision();
  const core::ProviderContractRefDTO hard_provider{
      core::ProviderContractRoleId(
          std::string(detail::kNativeSoftwareHardProviderContractRoleId)),
      core::SchemaId(
          std::string(detail::kNativeSoftwareHardContractSchemaId)),
      core::SchemaRevision(
          std::string(detail::kNativeSoftwareHardContractSchemaRevision))};
  const core::ProviderContractRefDTO controlled_provider{
      core::ProviderContractRoleId(
          std::string(detail::kNativeControlledStopProviderContractRoleId)),
      core::SchemaId(std::string(core::kMotionControlledStopContractSchemaId)),
      core::SchemaRevision(
          std::string(core::kMotionControlledStopContractSchemaRevision))};

  const auto provider_max = run_obstruction_case(ObstructionCase::kProviderCall);
  const auto lock_max = run_obstruction_case(ObstructionCase::kAdapterLock);
  const auto wait_max = run_obstruction_case(ObstructionCase::kResultWait);
  const auto substitution = run_real_substitution();

  const deployment_detail::BuildQualifiedStopImplementationEvidenceEntry
      implementation_entry{
          implementation_id,
          core::ContentDigest(implementation_fingerprint),
          hard_provider,
          core::ProductContractId(std::string(
              deployment::native_execution_runtime_contract::kProductContractId)),
          provider_revision,
          core::RegisteredStopCommandKindId(
              "manyforge.stop.software_hard"),
          std::nullopt,
          core::StopInvocationLaneId(
              std::string(detail::kNativeSoftwareHardStopInvocationLaneId)),
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
  const deployment_detail::
      BuildQualifiedControlledStopSubstitutionEvidenceEntry substitution_entry{
          implementation_id,
          core::ContentDigest(implementation_fingerprint),
          controlled_provider,
          core::ProductContractId(std::string(
              deployment::native_execution_runtime_contract::kProductContractId)),
          provider_revision,
          core::MotionControlledStopMode::kSoftOnPath,
          core::MotionControlledStopMode::kSpringBackHold,
          substitution.cases};

  const auto implementation_package =
      deployment_detail::emit_stop_implementation_qualification_evidence_package(
          core::ContentDigest(runner_digest),
          std::span<const deployment_detail::
                        BuildQualifiedStopImplementationEvidenceEntry>(
              &implementation_entry, 1U));
  const auto substitution_package = deployment_detail::
      emit_controlled_stop_substitution_qualification_evidence_package(
          core::ContentDigest(runner_digest),
          std::span<const deployment_detail::
                        BuildQualifiedControlledStopSubstitutionEvidenceEntry>(
              &substitution_entry, 1U));
  write_atomic(arguments.implementation_output, implementation_package);
  write_atomic(arguments.substitution_output, substitution_package);
  return 0;
}

}  // namespace
}  // namespace manyforge::roboplan_backend

int main(int argc, char** argv) {
  try {
    return manyforge::roboplan_backend::run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "native Stop qualification failed: " << error.what()
              << std::endl;
    return 1;
  }
}
