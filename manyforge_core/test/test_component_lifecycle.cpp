#include <gtest/gtest.h>

#include <manyforge_core/types/component_lifecycle.hpp>
#include <string>
#include <type_traits>

namespace manyforge::core {
namespace {

TEST(ComponentLifecycleStrongIds, PreserveSemanticSeparation) {
  static_assert(!std::is_same_v<LifecycleFactId, LifecycleTransactionId>);
  static_assert(!std::is_same_v<ComponentLifecycleGeneration, LifecycleTransactionGeneration>);
}

}  // namespace
}  // namespace manyforge::core
