#pragma once

#include <atomic>
#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <deque>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::ros2_control_execution {

// Diagnostic ownership only. Tokens never retain a trajectory or transport.
class VolatileMotionTrace final {
public:
  using Goal = control_msgs::action::FollowJointTrajectory::Goal;
  struct Context {
    std::string operation_id, participant_id, program_run_id,
        program_binding_id, component_id;
    std::uint64_t operation_revision{};
  };
  struct Token final {
    std::string action;
    core::ExecutionBindingEpoch epoch;
    core::ExecutionGoalKey key;
    std::optional<Context> context;
    std::shared_ptr<void> allocation_charge{};
  };
  using TokenPtr = std::shared_ptr<const Token>;
  using Garbage = std::vector<std::shared_ptr<const Goal>>;
  static constexpr std::size_t kGoalLimit = 16U;
  static constexpr std::size_t kGoalBytes = 4U * 1024U * 1024U;
  static constexpr std::size_t kSampleLimit = 1024U;
  static constexpr std::size_t kSampleBytes = 1024U * 1024U;
  static constexpr std::size_t kMaximumTraceBytes = 3U * 1024U * 1024U;

  void Preregister(
      const std::string &action, core::ExecutionBindingEpoch epoch,
      const core::ExecutionGoalKey &key,
      const core::PreparedOperationParticipantDTO &participant) noexcept;
  TokenPtr Begin(const std::string &action, core::ExecutionBindingEpoch epoch,
                 const core::ExecutionGoalKey &key, const TokenPtr &predecessor,
                 core::TimePoint attempted_at, Garbage &garbage) noexcept;
  void Attach(const TokenPtr &token, Goal &&goal, core::TimePoint returned_at,
              core::ExecutionPortDispatch disposition,
              Garbage &garbage) noexcept;
  void Observe(const TokenPtr &token,
               const core::ExecutionGoalObservationDTO &facts) noexcept;
  void Sample(const std::string &action,
              const core::ExecutionControllerSampleDTO &sample) noexcept;
  void PublishedScale(const std::string &action,
                      core::ExecutionBindingEpoch epoch, core::TimePoint when,
                      double value) noexcept;
  core::Result<content_identity::CanonicalValue>
  Snapshot(const core::RequestOwnerEpoch &owner,
           std::size_t allowance_bytes) const;

private:
  struct Pending {
    Token token;
    Context context;
  };
  struct Entry {
    TokenPtr token;
    std::optional<Context> context;
    TokenPtr predecessor;
    core::TimePoint attempted_at{};
    std::optional<core::TimePoint> returned_at;
    core::ExecutionPortDispatch disposition{
        core::ExecutionPortDispatch::kEffectUncertain};
    core::ExecutionGoalObservationDTO facts;
    std::optional<core::TimePoint> first_own_feedback;
    std::shared_ptr<const Goal> payload;
    std::size_t bytes{};
  };
  struct SampleEntry {
    std::string action;
    core::ExecutionControllerSampleDTO sample;
    std::size_t bytes;
  };
  struct ScaleEntry {
    std::string action;
    core::ExecutionBindingEpoch epoch;
    core::TimePoint when;
    double factor;
    bool published;
  };
  static bool Matches(const Token &a, const Token &b) noexcept;
  void Evict(Garbage &garbage);
  std::shared_ptr<void> Charge(std::size_t bytes);
  // Includes callback tokens, evicted garbage and snapshot-pinned payloads.
  std::shared_ptr<std::atomic<std::size_t>> allocated_ =
      std::make_shared<std::atomic<std::size_t>>(0U);
  mutable std::mutex mutex_;
  // Exactly one bounded snapshot/serialization scratch owner at a time.
  mutable std::mutex snapshot_mutex_;
  std::deque<Pending> pending_;
  std::deque<Entry> goals_;
  std::deque<SampleEntry> samples_;
  std::deque<ScaleEntry> scales_;
  std::size_t goal_bytes_{}, sample_bytes_{};
  std::uint64_t evicted_{}, omitted_{}, dropped_samples_{}, evicted_scales_{};
  mutable std::atomic<std::uint64_t> failures_{0};
};
} // namespace manyforge::ros2_control_execution
