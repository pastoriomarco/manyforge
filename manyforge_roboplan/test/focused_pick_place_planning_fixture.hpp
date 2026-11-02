#pragma once

#include <manyforge_core/pick_place_planning_owner.hpp>
#include <memory>

namespace manyforge::roboplan_backend::process_fixture {

/// Test-only subordinate ports for the focused native transport qualification.
///
/// The returned planning owner is the production C++ owner. The opaque
/// lifetime keeps deterministic, non-effectful subordinate ports alive while
/// the test-only native host serves requests. No test fixture is linked into
/// the installed execution host.
struct FocusedPickPlacePlanningFixture final {
  std::shared_ptr<void> lifetime;
  std::unique_ptr<core::AddressedPickPlacePlanningOwner> owner;
};

[[nodiscard]] FocusedPickPlacePlanningFixture
MakeFocusedPickPlacePlanningFixture();

} // namespace manyforge::roboplan_backend::process_fixture
