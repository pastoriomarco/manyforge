#include <manyforge_deployment/retained_bundle.hpp>

#include <type_traits>

static_assert(
    std::is_abstract_v<manyforge::deployment::IRawArtifactStore>);

