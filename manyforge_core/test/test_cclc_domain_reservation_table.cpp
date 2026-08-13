#include <gtest/gtest.h>

#include <manyforge_core/cclc_domain_reservation_table.hpp>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

[[nodiscard]] CCLCDomainReservationOwnerDTO LifecycleOwner(
    std::string transaction = "transaction.1", std::uint64_t generation = 1U,
    std::string component = "component.1") {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          ControllableComponentId(std::move(component)),
          LifecycleTransactionId(std::move(transaction)),
          LifecycleTransactionGeneration(generation),
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] CCLCDomainReservationOwnerDTO RouteOwner(
    std::string transaction = "route-transaction.1", std::uint64_t generation = 1U,
    std::vector<ControllableComponentId> components = {ControllableComponentId("component.1")}) {
  return CCLCDomainReservationOwnerDTO(RouteSelectionDomainReservationOwnerDTO{
      RouteSelectionTransactionId(std::move(transaction)),
      RouteSelectionTransactionGeneration(generation), std::move(components)});
}

[[nodiscard]] std::vector<ControlDomainId> Domains(std::initializer_list<const char*> values) {
  std::vector<ControlDomainId> result;
  for (const auto* value : values) {
    result.emplace_back(value);
  }
  return result;
}

TEST(CCLCDomainReservationTable, AcquiresTheCompleteCanonicalScopeAndReplaysExactly) {
  CCLCDomainReservationTable table;
  const auto owner = LifecycleOwner();
  const auto domains = Domains({"domain.a", "domain.b", "domain.c"});

  ASSERT_TRUE(table.reserve(owner, domains).ok());
  ASSERT_TRUE(table.reserve(owner, domains).ok());
  const auto snapshot = table.snapshot();
  ASSERT_EQ(snapshot.size(), domains.size());
  for (std::size_t index = 0U; index < snapshot.size(); ++index) {
    EXPECT_EQ(snapshot[index].control_domain_id, domains[index]);
    EXPECT_EQ(snapshot[index].owner, owner);
  }
}

TEST(CCLCDomainReservationTable, LifecycleAndRouteSelectionShareOneOverlapAuthority) {
  CCLCDomainReservationTable table;
  ASSERT_TRUE(table.reserve(LifecycleOwner(), Domains({"domain.b"})).ok());

  const auto overlap = table.reserve(RouteOwner(), Domains({"domain.a", "domain.b"}));
  ASSERT_FALSE(overlap.ok());
  EXPECT_EQ(overlap.error().code, "cclc_domain_busy");
  ASSERT_EQ(table.snapshot().size(), 1U);

  // Generation counters are typed and independent across request families.
  ASSERT_TRUE(table.reserve(RouteOwner(), Domains({"domain.c"})).ok());
  EXPECT_EQ(table.snapshot().size(), 2U);
}

TEST(CCLCDomainReservationTable, AllowsDisjointOwnersAndReleasesOnlyTheExactScope) {
  CCLCDomainReservationTable table;
  const auto first = LifecycleOwner();
  const auto second =
      RouteOwner("route-transaction.2", 2U, {ControllableComponentId("component.2")});
  const auto first_domains = Domains({"domain.a", "domain.b"});
  const auto second_domains = Domains({"domain.c"});
  ASSERT_TRUE(table.reserve(first, first_domains).ok());
  ASSERT_TRUE(table.reserve(second, second_domains).ok());

  EXPECT_FALSE(table.release(first, Domains({"domain.a"})).ok());
  EXPECT_EQ(table.snapshot().size(), 3U);
  ASSERT_TRUE(table.release(first, first_domains).ok());
  const auto snapshot = table.snapshot();
  ASSERT_EQ(snapshot.size(), 1U);
  EXPECT_EQ(snapshot.front().owner, second);
}

TEST(CCLCDomainReservationTable, RejectsTaggedIdentityReuseAndNoncanonicalScope) {
  CCLCDomainReservationTable table;
  ASSERT_TRUE(table.reserve(LifecycleOwner(), Domains({"domain.a"})).ok());

  const auto reused_id =
      table.reserve(LifecycleOwner("transaction.1", 2U, "component.2"), Domains({"domain.b"}));
  ASSERT_FALSE(reused_id.ok());
  EXPECT_EQ(reused_id.error().details.at("reason"), "transaction_identity_reused");
  const auto reused_generation =
      table.reserve(LifecycleOwner("transaction.2", 1U, "component.2"), Domains({"domain.b"}));
  ASSERT_FALSE(reused_generation.ok());
  EXPECT_EQ(reused_generation.error().details.at("reason"), "transaction_identity_reused");

  EXPECT_FALSE(
      table.reserve(LifecycleOwner("transaction.3", 3U), Domains({"domain.b", "domain.a"})).ok());
  EXPECT_FALSE(
      table.reserve(LifecycleOwner("transaction.3", 3U), Domains({"domain.a", "domain.a"})).ok());

  auto malformed = RouteOwner();
  malformed.request_kind = CCLCRequestKindDTO::kComponentLifecycle;
  EXPECT_FALSE(malformed.locally_valid());

  EXPECT_FALSE(RouteOwner("route.empty", 3U, {}).locally_valid());
  EXPECT_FALSE(
      RouteOwner("route.duplicate", 3U,
                 {ControllableComponentId("component.1"), ControllableComponentId("component.1")})
          .locally_valid());
  EXPECT_TRUE(
      RouteOwner("route.complete", 3U,
                 {ControllableComponentId("component.1"), ControllableComponentId("component.2")})
          .locally_valid());
}

TEST(CCLCDomainReservationTable, EnforcesTheSharedActiveTransactionCeiling) {
  CCLCDomainReservationTable table;
  for (std::size_t index = 0U; index < kMaxCCLCActiveDomainReservations; ++index) {
    const auto accepted =
        table.reserve(LifecycleOwner("transaction." + std::to_string(index + 1U), index + 1U,
                                     "component." + std::to_string(index + 1U)),
                      {ControlDomainId("domain." + std::to_string(index + 1U))});
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  }

  const auto full = table.reserve(
      RouteOwner("route-transaction.overflow", 1U, {ControllableComponentId("component.overflow")}),
      Domains({"domain.overflow"}));
  ASSERT_FALSE(full.ok());
  EXPECT_EQ(full.error().code, "request_queue_full");
}

}  // namespace
}  // namespace manyforge::core
