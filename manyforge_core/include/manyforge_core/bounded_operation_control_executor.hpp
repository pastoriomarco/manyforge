#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumOperationControlLanes = 4096U;

struct OperationControlEffectHandle final {
  std::uint32_t slot{};
  std::uint64_t generation{};

  [[nodiscard]] bool locally_valid() const noexcept { return generation != 0U; }
  friend bool operator==(const OperationControlEffectHandle&,
                         const OperationControlEffectHandle&) = default;
};

/// One already-durably-accepted participant-local control effect. Reset reaches
/// this boundary only after the Kernel's ordinary scoped Stop owner has
/// certified containment. Capability-command acknowledgement needs no
/// subordinate effect and is settled by the aggregate owner directly.
struct OperationControlEffectRequest final {
  IResolvedParticipantExecutionEngine* engine{};
  OperationId operation_id;
  OperationParticipantId participant_id;
  OperationRevision operation_revision{0U};
  OperationControlKind control{OperationControlKind::kPause};
  std::uint64_t subordinate_id{};
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  std::string command_target;
  std::uint64_t subordinate_generation{};
  IResolvedPreparedParticipantExecutionEngine* prepared_engine{};
  std::shared_ptr<const ResolvedPreparedMotionResumeRequestDTO> prepared_resume;
  std::shared_ptr<std::atomic_bool> cancellation_requested;
};

struct OperationControlEffectResult final {
  OperationId operation_id;
  OperationParticipantId participant_id;
  OperationRevision operation_revision{0U};
  OperationControlKind control{OperationControlKind::kPause};
  bool accepted{};
  std::optional<ErrorDTO> error;
  // Resume-only cleanup facts. A created preparation that is not yet cleaned
  // up must remain owned until either dispatch is linearized or exact
  // pre-dispatch cancellation completes.
  bool prepared_resume_created{};
  bool prepared_resume_cleanup_complete{};
};

enum class OperationControlEffectStartStatus : std::uint8_t {
  kStarted = 0,
  kInvalidRequest,
  kClosed,
  kNotPrequalified,
  kLaneBusy,
};

struct OperationControlEffectStartResult final {
  OperationControlEffectStartStatus status{OperationControlEffectStartStatus::kInvalidRequest};
  std::optional<OperationControlEffectHandle> handle;
};

enum class OperationControlEffectTakeStatus : std::uint8_t {
  kPending = 0,
  kCompleted,
  kInfrastructureFailure,
  kAlreadyTaken,
  kStaleHandle,
};

struct OperationControlEffectTakeResult final {
  OperationControlEffectTakeStatus status{OperationControlEffectTakeStatus::kStaleHandle};
  std::optional<OperationControlEffectResult> result;
};

enum class OperationControlEffectReleaseStatus : std::uint8_t {
  kReleased = 0,
  kNotTaken,
  kStaleHandle,
};

class BoundedOperationControlExecutor;

class IOperationControlEffectCompletionReceiver {
 public:
  virtual ~IOperationControlEffectCompletionReceiver() = default;
  virtual void OperationControlEffectCompleted(BoundedOperationControlExecutor& executor,
                                               OperationControlEffectHandle handle) noexcept = 0;
};

/// Fixed process-local isolation for participant Pause/Resume and completed-
/// execution retirement. Construction creates exactly one worker lane per
/// distinct registered subordinate engine. This preserves per-engine command
/// ordering while allowing disjoint participants to progress independently.
///
/// Public control admission publishes only into a precreated free lane and
/// never waits for controller/vendor completion. Registered engines and the
/// completion receiver must outlive successful CloseIngress()+DrainFor().
class BoundedOperationControlExecutor final {
 public:
  BoundedOperationControlExecutor(std::vector<IResolvedParticipantExecutionEngine*> engines,
                                  IOperationControlEffectCompletionReceiver& receiver);
  ~BoundedOperationControlExecutor();

  BoundedOperationControlExecutor(const BoundedOperationControlExecutor&) = delete;
  BoundedOperationControlExecutor& operator=(const BoundedOperationControlExecutor&) = delete;
  BoundedOperationControlExecutor(BoundedOperationControlExecutor&&) = delete;
  BoundedOperationControlExecutor& operator=(BoundedOperationControlExecutor&&) = delete;

  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] bool prequalified_ready() const noexcept;
  [[nodiscard]] OperationControlEffectStartResult TryStart(
      OperationControlEffectRequest request) noexcept;
  [[nodiscard]] OperationControlEffectTakeResult TryTake(
      OperationControlEffectHandle handle) noexcept;
  [[nodiscard]] OperationControlEffectReleaseStatus Release(
      OperationControlEffectHandle handle) noexcept;
  void CloseIngress() noexcept;
  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout) noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
