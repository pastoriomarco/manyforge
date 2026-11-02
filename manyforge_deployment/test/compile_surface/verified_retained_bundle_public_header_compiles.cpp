#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <filesystem>
#include <type_traits>

using Bundle = manyforge::deployment::RetainedBundle;
using BundleSource =
    manyforge::deployment::IVerifiedRetainedBundleSource;
using FilesystemBundleSource =
    manyforge::deployment::FilesystemVerifiedRetainedBundleSource;
using ProductSource =
    manyforge::deployment::IVerifiedRetainedProductArtifactSource;
using ModelMapSource =
    manyforge::deployment::IVerifiedRetainedModelMapSource;
using ModelMapFilesystemSource =
    manyforge::deployment::FilesystemVerifiedRetainedModelMapSource;
using ModelMap = manyforge::deployment::VerifiedRetainedModelMap;
using PlanningSource =
    manyforge::deployment::IVerifiedRetainedPlanningModelSource;
using PlanningFilesystemSource =
    manyforge::deployment::FilesystemVerifiedRetainedPlanningModelSource;
using PlanningModel =
    manyforge::deployment::VerifiedRetainedPlanningModel;
using PlanningResource =
    manyforge::deployment::VerifiedRetainedPlanningResource;
using ProjectionInput =
    manyforge::deployment::VerifiedRetainedProjectionInput;

static_assert(std::is_final_v<Bundle>);
static_assert(!std::is_default_constructible_v<Bundle>);
static_assert(!std::is_copy_constructible_v<Bundle>);
static_assert(std::is_abstract_v<BundleSource>);
static_assert(std::is_abstract_v<ProductSource>);
static_assert(std::is_abstract_v<ModelMapSource>);
static_assert(std::is_abstract_v<PlanningSource>);
static_assert(std::is_base_of_v<BundleSource, FilesystemBundleSource>);
static_assert(std::is_base_of_v<PlanningSource, PlanningFilesystemSource>);
static_assert(std::is_base_of_v<ModelMapSource, ModelMapFilesystemSource>);
static_assert(!std::is_default_constructible_v<ModelMap>);
static_assert(!std::is_aggregate_v<ModelMap>);
static_assert(std::is_copy_constructible_v<ModelMap>);
static_assert(std::is_move_constructible_v<ModelMap>);
static_assert(!std::is_default_constructible_v<PlanningModel>);
static_assert(!std::is_default_constructible_v<PlanningResource>);
static_assert(std::is_copy_constructible_v<PlanningModel>);
static_assert(std::is_move_constructible_v<PlanningModel>);
static_assert(!std::is_default_constructible_v<ProjectionInput>);
static_assert(!std::is_aggregate_v<ProjectionInput>);
static_assert(std::is_copy_constructible_v<ProjectionInput>);
static_assert(std::is_move_constructible_v<ProjectionInput>);
static_assert(std::is_constructible_v<
              FilesystemBundleSource, std::filesystem::path>);
static_assert(std::is_constructible_v<
              PlanningFilesystemSource, std::filesystem::path>);
static_assert(std::is_constructible_v<
              ModelMapFilesystemSource, std::filesystem::path>);
