#include <manyforge_deployment/canonical_empty_admission.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

using Admission =
    manyforge::deployment::CanonicalEmptyRetainedBundleAdmission;
using Source =
    manyforge::deployment::ICanonicalEmptyRetainedBundleSource;
using FilesystemSource =
    manyforge::deployment::FilesystemCanonicalEmptyRetainedBundleSource;

static_assert(std::is_final_v<Admission>);
static_assert(!std::is_aggregate_v<Admission>);
static_assert(!std::is_default_constructible_v<Admission>);
static_assert(!std::is_copy_constructible_v<Admission>);
static_assert(!std::is_copy_assignable_v<Admission>);
static_assert(!std::is_move_constructible_v<Admission>);
static_assert(!std::is_move_assignable_v<Admission>);
static_assert(!std::is_constructible_v<
              Admission,
              manyforge::core::ContentDigest,
              manyforge::core::CellId,
              manyforge::core::DeploymentArtifactId,
              manyforge::core::DeploymentRevision,
              manyforge::core::GraphRevision,
              manyforge::core::RetainedBundleId,
              manyforge::core::ContentDigest,
              manyforge::core::ContentDigest,
              manyforge::core::ContentDigest,
              std::shared_ptr<
                  const manyforge::deployment::RetainedBundle>,
              std::string>);
static_assert(std::is_abstract_v<Source>);
static_assert(std::is_base_of_v<Source, FilesystemSource>);
static_assert(std::is_constructible_v<
              FilesystemSource, std::filesystem::path>);
static_assert(!std::is_constructible_v<
              FilesystemSource, std::filesystem::path,
              manyforge::core::CellId>);
static_assert(std::is_same_v<
              decltype(std::declval<const Source&>().load_verified()),
              Admission>);
