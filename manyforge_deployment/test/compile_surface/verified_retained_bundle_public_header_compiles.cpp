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

static_assert(std::is_final_v<Bundle>);
static_assert(!std::is_default_constructible_v<Bundle>);
static_assert(!std::is_copy_constructible_v<Bundle>);
static_assert(std::is_abstract_v<BundleSource>);
static_assert(std::is_abstract_v<ProductSource>);
static_assert(std::is_base_of_v<BundleSource, FilesystemBundleSource>);
static_assert(std::is_constructible_v<
              FilesystemBundleSource, std::filesystem::path>);
