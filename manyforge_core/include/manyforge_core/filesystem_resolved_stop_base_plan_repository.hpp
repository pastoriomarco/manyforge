#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <manyforge_core/ports/iresolved_stop_base_plan_repository.hpp>
#include <memory>

namespace manyforge::core {

inline constexpr std::size_t kDefaultResolvedStopBasePlanRepositoryCount = 4096U;
inline constexpr std::uint64_t kDefaultResolvedStopBasePlanRepositoryBytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumResolvedStopBasePlanEncodedBytes = 16ULL * 1024ULL * 1024ULL;

/// POSIX durable immutable repository for resolved Stop base plans.
///
/// `trusted_root` must already be an existing deployment-owned plain
/// directory whose ancestors are not concurrently replaced. The repository
/// durably owns `resolved-stop-base-plans/sha256/` below that root. Instances
/// are process-affine: after `fork()`, the child must construct a fresh
/// instance before use.
class FilesystemResolvedStopBasePlanRepository final : public IResolvedStopBasePlanRepository {
 public:
  explicit FilesystemResolvedStopBasePlanRepository(
      std::filesystem::path trusted_root,
      std::size_t maximum_plan_count = kDefaultResolvedStopBasePlanRepositoryCount,
      std::uint64_t maximum_total_bytes = kDefaultResolvedStopBasePlanRepositoryBytes,
      std::uint64_t maximum_single_plan_bytes = kMaximumResolvedStopBasePlanEncodedBytes);
  ~FilesystemResolvedStopBasePlanRepository() override;

  FilesystemResolvedStopBasePlanRepository(const FilesystemResolvedStopBasePlanRepository&) =
      delete;
  FilesystemResolvedStopBasePlanRepository& operator=(
      const FilesystemResolvedStopBasePlanRepository&) = delete;
  FilesystemResolvedStopBasePlanRepository(FilesystemResolvedStopBasePlanRepository&&) = delete;
  FilesystemResolvedStopBasePlanRepository& operator=(FilesystemResolvedStopBasePlanRepository&&) =
      delete;

  [[nodiscard]] Result<ResolvedStopBasePlanId> PutIfAbsent(ResolvedStopBasePlanDTO plan) override;

  [[nodiscard]] Result<ResolvedStopBasePlanDTO> LoadVerified(
      const ResolvedStopBasePlanId& resolved_stop_base_plan_id) const override;

 private:
  class Storage;
  std::unique_ptr<Storage> storage_;
};

}  // namespace manyforge::core
