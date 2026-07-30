#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <cstddef>
#include <concepts>
#include <exception>
#include <fstream>
#include <iterator>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

template <typename Repository>
concept HasActiveAliasMutation =
    requires(Repository &repository, const core::RetainedBundleId &bundle_id) {
      repository.set_active(bundle_id);
    };

static_assert(!HasActiveAliasMutation<IRetainedBundleRepository>);
static_assert(!std::is_aggregate_v<BuildQualifiedProductContract>);
static_assert(!std::is_default_constructible_v<BuildQualifiedProductContract>);
static_assert(!std::is_aggregate_v<RetainedBundle>);
static_assert(!std::is_default_constructible_v<RetainedBundle>);
static_assert(!std::is_constructible_v<RetainedBundle, core::RetainedBundleId,
                                       CanonicalValue, std::string>);
static_assert(!std::is_copy_constructible_v<InMemoryRawArtifactStore>);
static_assert(!std::is_copy_assignable_v<InMemoryRawArtifactStore>);
static_assert(!std::is_move_constructible_v<InMemoryRawArtifactStore>);
static_assert(!std::is_move_assignable_v<InMemoryRawArtifactStore>);
static_assert(
    !std::is_copy_constructible_v<InMemoryRetainedBundleRepository>);
static_assert(!std::is_copy_assignable_v<InMemoryRetainedBundleRepository>);
static_assert(
    !std::is_move_constructible_v<InMemoryRetainedBundleRepository>);
static_assert(!std::is_move_assignable_v<InMemoryRetainedBundleRepository>);

TEST(RetainedBundlePublicSurfaceTest,
     DoesNotExposeArbitrarySemanticProductRetention) {
  std::ifstream header(MANYFORGE_RETAINED_BUNDLE_PUBLIC_HEADER);
  ASSERT_TRUE(header.is_open());
  const std::string source((std::istreambuf_iterator<char>(header)),
                           std::istreambuf_iterator<char>());

  for (const std::string_view removed_name :
       {"RetainedArtifactInput"sv, "build_and_retain_product"sv,
        "retained_product_projection"sv,
        "retained_product_output_digest"sv}) {
    EXPECT_EQ(source.find(removed_name), std::string::npos) << removed_name;
  }
}

[[nodiscard]] const BuildQualifiedProductContract &flattened_contract() {
  const auto contracts =
      build_qualified_product_contract_registry().contracts();
  if (contracts.size() != 1U) {
    throw std::logic_error(
        "test requires exactly one qualified product contract");
  }
  return contracts.front();
}

[[nodiscard]] std::vector<detail::RetainedArtifactInput>
product_contract_fixture_artifacts() {
  return {
      {core::ArtifactRoleId("urdf"), "application/xml",
       "<robot name=\"fixture\"><link name=\"fixture_link\"/></robot>"},
      {core::ArtifactRoleId("srdf"), "application/xml",
       "<robot name=\"fixture\"><group name=\"fixture_group\"><link "
       "name=\"fixture_link\"/></group></robot>"},
  };
}

[[nodiscard]] core::RetainedProductDTO
retain_fixture_product(IRawArtifactStore &store,
                       std::vector<detail::RetainedArtifactInput> artifacts =
                           product_contract_fixture_artifacts()) {
  const auto &contract = flattened_contract();
  return detail::build_and_retain_product(
      core::ProductId("cell_planning_collision_model"),
      core::RetainedProductKind::kPlanningCollisionModel,
      ProductPlacement::kCellPlanningCollisionModel,
      contract.product_contract_id(), contract.product_contract_revision(),
      artifacts, store);
}

enum class StoreFault {
  kForgedDigest,
  kMissingReadBack,
  kChangedReadBack,
};

class AdversarialRawArtifactStore final : public IRawArtifactStore {
public:
  explicit AdversarialRawArtifactStore(StoreFault fault) : fault_(fault) {}

  [[nodiscard]] StoredRawArtifact
  put_if_absent(std::string_view bytes) override {
    ++put_calls_;
    bytes_ = std::string(bytes);
    std::string digest = sha256_digest(bytes);
    if (fault_ == StoreFault::kForgedDigest) {
      digest = std::string(kZeroDigest);
    }
    return {
        core::ContentDigest(digest),
        core::ContentAddressedStoreKey("sha256/" + digest.substr(7U)),
        static_cast<std::uint64_t>(bytes.size()),
        true,
    };
  }

  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &,
               const core::ContentDigest &, std::uint64_t) const override {
    ++get_calls_;
    if (fault_ == StoreFault::kMissingReadBack) {
      return std::nullopt;
    }
    if (fault_ == StoreFault::kChangedReadBack) {
      std::string changed = bytes_;
      if (!changed.empty()) {
        changed.front() = changed.front() == 'x' ? 'y' : 'x';
      }
      return changed;
    }
    return bytes_;
  }

  [[nodiscard]] std::size_t put_calls() const noexcept { return put_calls_; }
  [[nodiscard]] std::size_t get_calls() const noexcept { return get_calls_; }

private:
  StoreFault fault_;
  std::string bytes_;
  std::size_t put_calls_{0U};
  mutable std::size_t get_calls_{0U};
};

void expect_no_thread_failures(
    std::span<const std::exception_ptr> failures) {
  for (std::size_t index = 0U; index < failures.size(); ++index) {
    if (failures[index] == nullptr) {
      continue;
    }
    try {
      std::rethrow_exception(failures[index]);
    } catch (const std::exception &error) {
      ADD_FAILURE() << "worker " << index << " failed: " << error.what();
    } catch (...) {
      ADD_FAILURE() << "worker " << index
                    << " failed with a non-standard exception";
    }
  }
}

} // namespace

TEST(ProductContractRegistryTest, QualifiesOnlyExactFlattenedModelContract) {
  const auto &registry = build_qualified_product_contract_registry();
  const auto contracts = registry.contracts();
  ASSERT_EQ(contracts.size(), 1U);
  const auto &contract = contracts.front();

  EXPECT_EQ(contract.product_contract_id().value(),
            "manyforge.flattened_model");
  EXPECT_EQ(contract.product_contract_revision().value(),
            "sha256:"
            "fe0fe30d7098f15443e50a5f228246b628d24f6ebda8c477f28f5468c7e5e7ff");
  EXPECT_EQ(contract.product_kind(),
            core::RetainedProductKind::kPlanningCollisionModel);
  EXPECT_EQ(contract.placement(),
            ProductPlacement::kCellPlanningCollisionModel);
  ASSERT_EQ(contract.artifact_roles().size(), 2U);
  EXPECT_EQ(contract.artifact_roles()[0].artifact_role_id.value(), "srdf");
  EXPECT_EQ(contract.artifact_roles()[1].artifact_role_id.value(), "urdf");
  for (const auto &role : contract.artifact_roles()) {
    EXPECT_EQ(role.media_type, "application/xml");
    EXPECT_EQ(role.cardinality,
              ProductArtifactCardinality::kRequiredExactlyOne);
  }

  const auto *declared_revision =
      contract.normalized_descriptor().find_object_member(
          "product_contract_revision");
  ASSERT_NE(declared_revision, nullptr);
  EXPECT_EQ(std::get<std::string>(declared_revision->storage()),
            contract.product_contract_revision().value());

  EXPECT_EQ(registry.find(contract.product_contract_id(),
                          contract.product_contract_revision()),
            &contract);
  EXPECT_EQ(
      registry.find(contract.product_contract_id(),
                    core::ProductContractRevision(std::string(kZeroDigest))),
      nullptr);
  EXPECT_EQ(registry.find(core::ProductContractId("manyforge.unqualified"),
                          contract.product_contract_revision()),
            nullptr);
}

TEST(RawArtifactStoreTest, UsesImmutableDigestKeysAndReverifiesReads) {
  InMemoryRawArtifactStore store;
  const StoredRawArtifact first = store.put_if_absent("binary\0bytes"sv);
  EXPECT_TRUE(first.inserted);
  EXPECT_EQ(first.byte_size, 12U);
  EXPECT_EQ(first.content_addressed_store_key.value(),
            "sha256/" + first.content_digest.value().substr(7U));
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.retained_bytes(), 12U);

  const StoredRawArtifact duplicate = store.put_if_absent("binary\0bytes"sv);
  EXPECT_FALSE(duplicate.inserted);
  EXPECT_EQ(duplicate.content_digest, first.content_digest);
  EXPECT_EQ(store.size(), 1U);

  const auto bytes = store.get_verified(first.content_addressed_store_key,
                                        first.content_digest, first.byte_size);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(*bytes, "binary\0bytes"sv);
  EXPECT_FALSE(store.get_verified(first.content_addressed_store_key,
                                  core::ContentDigest(std::string(kZeroDigest)),
                                  first.byte_size));
  EXPECT_FALSE(store.get_verified(first.content_addressed_store_key,
                                  first.content_digest, first.byte_size + 1U));
}

TEST(RawArtifactStoreTest, AppliesEntryAndByteBoundsWithoutEviction) {
  EXPECT_THROW(InMemoryRawArtifactStore(0U, 1U, 1U), RetainedBundleError);
  EXPECT_THROW(InMemoryRawArtifactStore(1U, 0U, 1U), RetainedBundleError);
  EXPECT_THROW(InMemoryRawArtifactStore(1U, 1U, 2U), RetainedBundleError);

  InMemoryRawArtifactStore store(1U, 4U, 4U);
  EXPECT_TRUE(store.put_if_absent("abcd").inserted);
  EXPECT_FALSE(store.put_if_absent("abcd").inserted);
  EXPECT_THROW(static_cast<void>(store.put_if_absent("x")),
               RetainedBundleError);

  InMemoryRawArtifactStore single_bound(2U, 8U, 4U);
  EXPECT_THROW(static_cast<void>(single_bound.put_if_absent("abcde")),
               RetainedBundleError);
  EXPECT_EQ(single_bound.size(), 0U);
}

TEST(RawArtifactStoreTest,
     ConcurrentSameBytesAreIdempotentWithExactAccounting) {
  constexpr std::size_t kThreadCount = 16U;
  const std::string bytes = "one immutable concurrent artifact";
  InMemoryRawArtifactStore store(1U, bytes.size(), bytes.size());
  std::barrier<> start(static_cast<std::ptrdiff_t>(kThreadCount + 1U));
  std::vector<std::optional<StoredRawArtifact>> results(kThreadCount);
  std::vector<std::exception_ptr> failures(kThreadCount);
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    workers.emplace_back([&, index] {
      start.arrive_and_wait();
      try {
        results[index] = store.put_if_absent(bytes);
      } catch (...) {
        failures[index] = std::current_exception();
      }
    });
  }
  start.arrive_and_wait();
  for (auto &worker : workers) {
    worker.join();
  }

  expect_no_thread_failures(failures);
  ASSERT_TRUE(std::all_of(results.begin(), results.end(),
                          [](const auto &result) {
                            return result.has_value();
                          }));
  EXPECT_EQ(std::count_if(results.begin(), results.end(),
                          [](const auto &result) {
                            return result->inserted;
                          }),
            1);
  const StoredRawArtifact &identity = *results.front();
  for (const auto &result : results) {
    EXPECT_EQ(result->content_digest, identity.content_digest);
    EXPECT_EQ(result->content_addressed_store_key,
              identity.content_addressed_store_key);
    EXPECT_EQ(result->byte_size, bytes.size());
  }
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.retained_bytes(), bytes.size());
  EXPECT_EQ(store.get_verified(identity.content_addressed_store_key,
                               identity.content_digest, identity.byte_size),
            bytes);
}

TEST(RawArtifactStoreTest,
     ConcurrentDistinctBytesHaveExactCountAndByteAccounting) {
  constexpr std::size_t kThreadCount = 16U;
  std::vector<std::string> inputs;
  inputs.reserve(kThreadCount);
  std::uint64_t expected_total_bytes = 0U;
  std::size_t maximum_single_bytes = 0U;
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    inputs.push_back("distinct-artifact-" + std::to_string(index) + "-" +
                     std::string(index + 1U, 'x'));
    expected_total_bytes +=
        static_cast<std::uint64_t>(inputs.back().size());
    maximum_single_bytes =
        std::max(maximum_single_bytes, inputs.back().size());
  }

  InMemoryRawArtifactStore store(
      kThreadCount, expected_total_bytes,
      static_cast<std::uint64_t>(maximum_single_bytes));
  std::barrier<> start(static_cast<std::ptrdiff_t>(kThreadCount + 1U));
  std::vector<std::optional<StoredRawArtifact>> results(kThreadCount);
  std::vector<std::exception_ptr> failures(kThreadCount);
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    workers.emplace_back([&, index] {
      start.arrive_and_wait();
      try {
        results[index] = store.put_if_absent(inputs[index]);
      } catch (...) {
        failures[index] = std::current_exception();
      }
    });
  }
  start.arrive_and_wait();
  for (auto &worker : workers) {
    worker.join();
  }

  expect_no_thread_failures(failures);
  ASSERT_TRUE(std::all_of(results.begin(), results.end(),
                          [](const auto &result) {
                            return result.has_value();
                          }));
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    EXPECT_TRUE(results[index]->inserted);
    EXPECT_EQ(results[index]->byte_size, inputs[index].size());
    EXPECT_EQ(store.get_verified(results[index]->content_addressed_store_key,
                                 results[index]->content_digest,
                                 results[index]->byte_size),
              inputs[index]);
  }
  EXPECT_EQ(store.size(), kThreadCount);
  EXPECT_EQ(store.retained_bytes(), expected_total_bytes);
}

TEST(RetainedProductTest, UsesContractBeforeRetainingCanonicalArtifacts) {
  InMemoryRawArtifactStore store;
  auto reversed = product_contract_fixture_artifacts();
  std::reverse(reversed.begin(), reversed.end());
  const core::RetainedProductDTO product =
      retain_fixture_product(store, reversed);

  ASSERT_EQ(product.artifact_refs.size(), 2U);
  EXPECT_EQ(product.artifact_refs[0].artifact_role_id.value(), "srdf");
  EXPECT_EQ(product.artifact_refs[1].artifact_role_id.value(), "urdf");
  EXPECT_EQ(store.size(), 2U);
  EXPECT_EQ(std::get<std::string>(detail::retained_product_projection(product)
                                      .find_object_member("product_contract_id")
                                      ->storage()),
            "manyforge.flattened_model");

  const core::ContentDigest output_digest =
      detail::retained_product_output_digest(product);
  EXPECT_EQ(output_digest.value(),
            "sha256:"
            "a87a55c23c21484f64d996a56fe8ff8f0c132aa49b8b1511fc107d871a2419e9");
  EXPECT_NE(output_digest, product.artifact_refs[0].content_digest);
  EXPECT_NE(output_digest, product.artifact_refs[1].content_digest);

  InMemoryRawArtifactStore second_store;
  const core::RetainedProductDTO same = retain_fixture_product(second_store);
  EXPECT_EQ(product, same);
  EXPECT_EQ(output_digest, detail::retained_product_output_digest(same));
}

TEST(RetainedProductTest,
     OutputDigestCoversEveryContractValidSemanticField) {
  InMemoryRawArtifactStore store;
  const core::RetainedProductDTO original = retain_fixture_product(store);
  const core::ContentDigest original_digest =
      detail::retained_product_output_digest(original);

  core::RetainedProductDTO changed = original;
  changed.product_id = core::ProductId("another_planning_collision_model");
  EXPECT_NE(detail::retained_product_output_digest(changed), original_digest);

  changed = original;
  ++changed.artifact_refs.front().byte_size;
  EXPECT_NE(detail::retained_product_output_digest(changed), original_digest);

  // A content digest and its digest-derived key form one indivisible valid
  // semantic change. Changing either alone is rejected below.
  changed = original;
  const std::string replacement_digest =
      sha256_digest("same-length-replacement-artifact");
  changed.artifact_refs.front().content_digest =
      core::ContentDigest(replacement_digest);
  changed.artifact_refs.front().content_addressed_store_key =
      core::ContentAddressedStoreKey("sha256/" +
                                     replacement_digest.substr(7U));
  EXPECT_NE(detail::retained_product_output_digest(changed), original_digest);
}

TEST(RetainedProductTest,
     ContractOwnedOrIncoherentSemanticFieldMutationsFailClosed) {
  InMemoryRawArtifactStore store;
  const core::RetainedProductDTO original = retain_fixture_product(store);

  core::RetainedProductDTO changed = original;
  changed.product_kind = core::RetainedProductKind::kViewerModel;
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(changed)),
      RetainedBundleError);

  changed = original;
  changed.product_contract_id =
      core::ProductContractId("manyforge.unqualified");
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(changed)),
      RetainedBundleError);

  changed = original;
  changed.product_contract_revision =
      core::ProductContractRevision(std::string(kZeroDigest));
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(changed)),
      RetainedBundleError);

  changed = original;
  changed.artifact_refs.front().artifact_role_id =
      core::ArtifactRoleId("unqualified_role");
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(changed)),
      RetainedBundleError);

  changed = original;
  changed.artifact_refs.front().media_type = "text/xml";
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(changed)),
      RetainedBundleError);

  changed = original;
  changed.artifact_refs.front().content_digest =
      core::ContentDigest(std::string(kZeroDigest));
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(changed)),
      RetainedBundleError);

  changed = original;
  changed.artifact_refs.front().content_addressed_store_key =
      core::ContentAddressedStoreKey("sha256/" + std::string(64U, '0'));
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(changed)),
      RetainedBundleError);
}

TEST(RetainedProductTest, RejectsContractDefectsBeforeStoreEffects) {
  const auto &contract = flattened_contract();
  InMemoryRawArtifactStore store;

  auto missing = product_contract_fixture_artifacts();
  missing.pop_back();
  EXPECT_THROW(static_cast<void>(detail::build_and_retain_product(
                   core::ProductId("cell_planning_collision_model"),
                   core::RetainedProductKind::kPlanningCollisionModel,
                   ProductPlacement::kCellPlanningCollisionModel,
                   contract.product_contract_id(),
                   contract.product_contract_revision(), missing, store)),
               RetainedBundleError);
  EXPECT_EQ(store.size(), 0U);

  auto duplicate = product_contract_fixture_artifacts();
  duplicate.push_back(duplicate.front());
  EXPECT_THROW(static_cast<void>(detail::build_and_retain_product(
                   core::ProductId("cell_planning_collision_model"),
                   core::RetainedProductKind::kPlanningCollisionModel,
                   ProductPlacement::kCellPlanningCollisionModel,
                   contract.product_contract_id(),
                   contract.product_contract_revision(), duplicate, store)),
               RetainedBundleError);
  EXPECT_EQ(store.size(), 0U);

  auto wrong_media = product_contract_fixture_artifacts();
  wrong_media.front().media_type = "text/xml";
  EXPECT_THROW(static_cast<void>(detail::build_and_retain_product(
                   core::ProductId("cell_planning_collision_model"),
                   core::RetainedProductKind::kPlanningCollisionModel,
                   ProductPlacement::kCellPlanningCollisionModel,
                   contract.product_contract_id(),
                   contract.product_contract_revision(), wrong_media, store)),
               RetainedBundleError);
  EXPECT_EQ(store.size(), 0U);

  auto extra = product_contract_fixture_artifacts();
  extra.push_back({core::ArtifactRoleId("mesh"), "model/stl", "solid fixture"});
  EXPECT_THROW(static_cast<void>(detail::build_and_retain_product(
                   core::ProductId("cell_planning_collision_model"),
                   core::RetainedProductKind::kPlanningCollisionModel,
                   ProductPlacement::kCellPlanningCollisionModel,
                   contract.product_contract_id(),
                   contract.product_contract_revision(), extra, store)),
               RetainedBundleError);
  EXPECT_EQ(store.size(), 0U);

  EXPECT_THROW(
      static_cast<void>(detail::build_and_retain_product(
          core::ProductId("cell_planning_collision_model"),
          core::RetainedProductKind::kViewerModel,
          ProductPlacement::kCellPlanningCollisionModel,
          contract.product_contract_id(), contract.product_contract_revision(),
          product_contract_fixture_artifacts(), store)),
      RetainedBundleError);
  EXPECT_EQ(store.size(), 0U);
}

TEST(RetainedProductTest, RejectsArtifactCountBeforeCallingStore) {
  const auto &contract = flattened_contract();
  auto extra = product_contract_fixture_artifacts();
  extra.push_back({core::ArtifactRoleId("mesh"), "model/stl", "solid fixture"});
  AdversarialRawArtifactStore store(StoreFault::kForgedDigest);

  EXPECT_THROW(static_cast<void>(detail::build_and_retain_product(
                   core::ProductId("cell_planning_collision_model"),
                   core::RetainedProductKind::kPlanningCollisionModel,
                   ProductPlacement::kCellPlanningCollisionModel,
                   contract.product_contract_id(),
                   contract.product_contract_revision(), extra, store)),
               RetainedBundleError);
  EXPECT_EQ(store.put_calls(), 0U);
  EXPECT_EQ(store.get_calls(), 0U);
}

TEST(RetainedProductTest, RejectsForgedArtifactStoreMetadata) {
  AdversarialRawArtifactStore store(StoreFault::kForgedDigest);

  EXPECT_THROW(static_cast<void>(retain_fixture_product(store)),
               std::runtime_error);
  EXPECT_EQ(store.put_calls(), 1U);
  EXPECT_EQ(store.get_calls(), 0U);
}

TEST(RetainedProductTest, RequiresVerifiedExactArtifactReadBack) {
  AdversarialRawArtifactStore missing(StoreFault::kMissingReadBack);
  EXPECT_THROW(static_cast<void>(retain_fixture_product(missing)),
               std::runtime_error);
  EXPECT_EQ(missing.put_calls(), 1U);
  EXPECT_EQ(missing.get_calls(), 1U);

  AdversarialRawArtifactStore changed(StoreFault::kChangedReadBack);
  EXPECT_THROW(static_cast<void>(retain_fixture_product(changed)),
               std::runtime_error);
  EXPECT_EQ(changed.put_calls(), 1U);
  EXPECT_EQ(changed.get_calls(), 1U);
}

TEST(RetainedProductTest,
     ProjectionRejectsCallerConstructedContractInconsistency) {
  InMemoryRawArtifactStore store;
  core::RetainedProductDTO product = retain_fixture_product(store);

  product.product_kind = core::RetainedProductKind::kViewerModel;
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_projection(product)),
      RetainedBundleError);

  product = retain_fixture_product(store);
  product.artifact_refs.pop_back();
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_output_digest(product)),
      RetainedBundleError);

  product = retain_fixture_product(store);
  product.artifact_refs.front().media_type = "text/xml";
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_projection(product)),
      RetainedBundleError);

  product = retain_fixture_product(store);
  std::swap(product.artifact_refs[0], product.artifact_refs[1]);
  EXPECT_THROW(
      static_cast<void>(detail::retained_product_projection(product)),
      RetainedBundleError);
}

TEST(RetainedBundleRepositoryTest,
     RemainsEmptyAndHasNoActiveAliasBeforeQualifiedI2ProducerExists) {
  InMemoryRetainedBundleRepository repository;
  EXPECT_EQ(repository.size(), 0U);
  EXPECT_EQ(repository.retained_bytes(), 0U);
  EXPECT_EQ(
      repository.get_verified(core::RetainedBundleId(std::string(kZeroDigest))),
      nullptr);
  EXPECT_THROW(static_cast<void>(repository.put_if_absent(nullptr)),
               RetainedBundleError);
}

TEST(RetainedBundleRepositoryTest, AppliesRepositoryBounds) {
  EXPECT_THROW(InMemoryRetainedBundleRepository(0U, 1U), RetainedBundleError);
  EXPECT_THROW(InMemoryRetainedBundleRepository(1U, 0U), RetainedBundleError);
}

} // namespace manyforge::deployment
