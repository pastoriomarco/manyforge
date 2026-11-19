#pragma once

#include <array>
#include <atomic>
#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <deque>
#include <filesystem>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/types/application_identity.hpp>
#include <manyforge_core/types/prepared_operation_participant.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::ros2_control_execution {

// Diagnostic ownership only. Tokens never retain a trajectory or transport.
class VolatileMotionTrace final {
public:
  using Goal = control_msgs::action::FollowJointTrajectory::Goal;
  struct Context {
    core::OperationNumber operation_id{0U};
    core::OperationParticipantId participant_id{};
    /// Absent for manual work.
    std::optional<core::ProgramRunNumber> program_run_id;
    std::string program_binding_id, component_id;
    std::uint64_t operation_revision{};
  };
  struct Token final {
    std::string action;
    core::ExecutionBindingEpoch epoch;
    core::ControllerGoalId key;
    std::optional<Context> context;
    std::shared_ptr<void> allocation_charge{};
  };
  using TokenPtr = std::shared_ptr<const Token>;
  using Garbage = std::vector<std::shared_ptr<const Goal>>;
  static constexpr std::size_t kGoalLimit = 16U;
  static constexpr std::size_t kGoalBytes = 4U * 1024U * 1024U;
  // Controller samples live in a ring of compact rows reserved at
  // construction: `Sample` never allocates, frees or blocks.
  static constexpr std::size_t kSampleBytes = 1024U * 1024U;
  /// Joints per sampled vector (xArm 7, the largest supported arm); a sample
  /// with more is counted as a capture failure.
  static constexpr std::size_t kTraceJoints = 7U;
  /// Longest action name a sample or scale event keeps; longer is counted as
  /// a capture failure.
  static constexpr std::size_t kActionBytes = 255U;  // fits the uint8_t length
  static constexpr std::size_t kScaleLimit = 256U;
  static constexpr std::size_t kMaximumTraceBytes = 5U * 1024U * 1024U;

  void Preregister(
      const std::string &action, core::ExecutionBindingEpoch epoch,
      const core::ControllerGoalId &key,
      const core::PreparedOperationParticipantDTO &participant) noexcept;
  TokenPtr Begin(const std::string &action, core::ExecutionBindingEpoch epoch,
                 const core::ControllerGoalId &key, const TokenPtr &predecessor,
                 core::TimePoint attempted_at, Garbage &garbage) noexcept;
  void Attach(const TokenPtr &token, Goal &&goal, core::TimePoint returned_at,
              core::ExecutionPortDispatch disposition,
              Garbage &garbage, bool replacement = false,
              const std::optional<core::ExecutionReplacementStartDTO> &start = {}) noexcept;
  void Observe(const TokenPtr &token,
               const core::ExecutionGoalObservationDTO &facts) noexcept;
  void Sample(const std::string &action,
              const core::ExecutionControllerSampleDTO &sample) noexcept;
  void PublishedScale(const std::string &action,
                      core::ExecutionBindingEpoch epoch, core::TimePoint when,
                      double value) noexcept;
  // `application_start_id` is written once, at the trace's top level, as
  // its 16-hex wire form.
  core::Result<content_identity::CanonicalValue>
  Snapshot(core::ApplicationStartId application_start_id,
           std::size_t allowance_bytes) const;
  core::Result<content_identity::CanonicalValue>
  SnapshotFile(core::ApplicationStartId application_start_id,
               std::size_t allowance_bytes,
               const std::filesystem::path &directory) const;

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
    bool replacement{};
    std::optional<core::ExecutionReplacementStartDTO> replacement_start{};
  };
  struct ActionName {
    std::array<char, kActionBytes> chars{};
    std::uint8_t size{};
    std::string_view view() const noexcept { return {chars.data(), size}; }
    void assign(std::string_view text) noexcept;
  };
  // Fixed-capacity ring; `Push` overwrites the oldest entry when full.
  template <typename T> struct Ring {
    explicit Ring(std::size_t capacity) : slots(capacity) {}
    bool full() const noexcept { return size == slots.size(); }
    // i-th retained entry, oldest first.
    const T &operator[](std::size_t i) const noexcept {
      return slots[(next + slots.size() - size + i) % slots.size()];
    }
    T &Push() noexcept {
      T &slot = slots[next];
      next = (next + 1U) % slots.size();
      size += full() ? 0U : 1U;
      return slot;
    }
    std::vector<T> slots;
    std::size_t next{}, size{};
  };
  static constexpr std::size_t kSampleVectors = 7U;
  static constexpr std::uint8_t kAbsentVector = 0xFFU;
  static constexpr std::size_t kActionSlots = 16U;
  // Exported fields only. Vectors in export order: reference p/v/a,
  // commanded p/v, measured p/v; `lengths[i]` is `kAbsentVector` for an
  // absent optional vector.
  struct SampleRow {
    std::uint64_t epoch;
    core::TimePoint source_stamp_ns, local_receipt_stamp_ns;
    double cursor_s, applied_factor;
    std::array<double, kSampleVectors * kTraceJoints> values;
    std::array<std::uint8_t, kSampleVectors> lengths;
    std::uint8_t action;
    bool has_applied_factor;
  };
  // A sample row names its action by slot; a slot is free when no row names it.
  struct ActionSlot {
    ActionName name;
    std::uint32_t rows{};
  };
  struct ScaleEntry {
    ActionName action;
    std::uint64_t epoch;
    core::TimePoint when;
    double factor;
    bool published;
  };

public:
  static constexpr std::size_t kSampleCapacity = kSampleBytes / sizeof(SampleRow);

private:
  static bool Matches(const Token &a, const Token &b) noexcept;
  std::size_t ActionIndex(std::string_view action) noexcept;
  void PushScale(std::string_view action, std::uint64_t epoch,
                 core::TimePoint when, double factor, bool published) noexcept;
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
  Ring<SampleRow> samples_{kSampleCapacity};
  std::array<ActionSlot, kActionSlots> actions_{};
  Ring<ScaleEntry> scales_{kScaleLimit};
  std::size_t goal_bytes_{};
  std::uint64_t evicted_{}, omitted_{}, dropped_samples_{}, evicted_scales_{};
  mutable std::atomic<std::uint64_t> failures_{0};
};
} // namespace manyforge::ros2_control_execution
