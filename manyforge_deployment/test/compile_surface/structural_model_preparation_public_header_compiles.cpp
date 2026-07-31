#include <manyforge_deployment/structural_model_preparation.hpp>

#include <string>
#include <type_traits>

using Preparation =
    manyforge::deployment::BuildQualifiedStructuralModelPreparation;

static_assert(std::is_final_v<Preparation>);
static_assert(!std::is_aggregate_v<Preparation>);
static_assert(!std::is_default_constructible_v<Preparation>);
static_assert(std::is_copy_constructible_v<Preparation>);
static_assert(!std::is_constructible_v<
              Preparation,
              manyforge::core::RetainedGeneratorRecordDTO,
              manyforge::deployment::SourceVerifiedModelAsset,
              std::string>);
