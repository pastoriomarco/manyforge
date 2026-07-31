#include <manyforge_deployment/retained_bundle.hpp>

#include <type_traits>

static_assert(
    std::is_abstract_v<manyforge::deployment::IRawArtifactStore>);
static_assert(std::is_abstract_v<
              manyforge::deployment::
                  IVerifiedRetainedProductArtifactSource>);
static_assert(std::is_final_v<
              manyforge::deployment::
                  VerifiedRetainedProductArtifact>);
static_assert(std::is_final_v<
              manyforge::deployment::
                  FilesystemVerifiedRetainedProductArtifactSource>);
