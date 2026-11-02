#include <manyforge_deployment/assistant_settings_codec.hpp>

#include <type_traits>

using Candidate = manyforge::deployment::TypedAssistantSettingsCandidate;
using Registry =
    manyforge::deployment::BuildQualifiedAssistantInstrumentRegistry;
using Qualified = manyforge::deployment::QualifiedAssistantSettings;

static_assert(std::is_final_v<Candidate>);
static_assert(!std::is_default_constructible_v<Candidate>);
static_assert(std::is_copy_constructible_v<Candidate>);
static_assert(std::is_final_v<Registry>);
static_assert(!std::is_default_constructible_v<Registry>);
static_assert(std::is_final_v<Qualified>);
static_assert(!std::is_default_constructible_v<Qualified>);
static_assert(std::is_copy_constructible_v<Qualified>);
