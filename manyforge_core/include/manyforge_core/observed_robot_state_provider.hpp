#pragma once

#include <manyforge_core/ports/iobserved_robot_state_provider.hpp>

#include <atomic>
#include <cstdint>
#include <optional>

namespace manyforge::core {

class PublishedObservedRobotStateProvider final
    : public IObservedRobotStateProvider {
 public:
  explicit PublishedObservedRobotStateProvider(
      const ObservedRobotStatePublication& publication,
      std::optional<ObservedRobotStateAuthorityProfile>
          required_authority_profile = std::nullopt,
      const std::atomic<std::uint64_t>* authority_epoch = nullptr)
      : publication_(&publication),
        required_authority_profile_(required_authority_profile),
        authority_epoch_(authority_epoch) {}

  Result<RobotStateDTO> current_state(std::int64_t now_ns) const override;
  const ObservedRobotStatePublication* publication() const noexcept override {
    return publication_;
  }

 private:
  const ObservedRobotStatePublication* publication_;
  std::optional<ObservedRobotStateAuthorityProfile>
      required_authority_profile_;
  // Optional owner-controlled fail-closed generation for non-expiring
  // commanded or predicted authorities. Odd values are invalid; a read is
  // admissible only when the same even value brackets its copy. The owner must
  // outlive this provider.
  const std::atomic<std::uint64_t>* authority_epoch_ = nullptr;
};

}  // namespace manyforge::core
