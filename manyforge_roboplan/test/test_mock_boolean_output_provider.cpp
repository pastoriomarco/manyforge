#include <manyforge_roboplan/mock_boolean_output_provider.hpp>

#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_core/time/fake_clock.hpp>

namespace manyforge::roboplan_backend {
namespace {

const core::ContentDigest kDigest("sha256:" + std::string(64U, 'a'));

core::ResolvedLifecycleProviderOccurrenceDTO occurrence(std::string name) {
  return {core::ControlDomainId(name + ".domain"),
          core::ControlDomainRevision(kDigest.value()),
          core::ProviderInstanceId(name + ".provider"),
          core::ProviderManifestId("mock_output"),
          core::ProviderManifestRevision(kDigest.value()),
          core::RuntimeUnitId("io.host"),
          core::RuntimeEpoch("runtime.1"),
          kDigest};
}

core::IoSignalId signal(std::string component, std::string name = "output") {
  return {core::ControllableComponentId(std::move(component)), std::move(name)};
}

// Leaf unit fixtures use distinct shared control blocks; actual host callers
// supply the existing IoProviderDriverRecord control block instead.
std::shared_ptr<const void> owner(std::string_view name) {
  static const auto a = std::make_shared<const int>(1);
  static const auto b = std::make_shared<const int>(2);
  static const auto foreign = std::make_shared<const int>(3);
  if (name == "a")
    return a;
  if (name == "b")
    return b;
  return foreign;
}

template <class Alias>
std::vector<IoOutputAliasIdentity>
identities(const std::vector<Alias> &aliases) {
  std::vector<IoOutputAliasIdentity> result;
  for (const auto &alias : aliases)
    result.push_back(
        {alias.signal_id, alias.provider, alias.actual_driver_owner});
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return a.signal_id < b.signal_id;
  });
  return result;
}

MockBooleanOutputChannel channel() {
  return {core::ResourceId("mock.output.shared"),
          "shared",
          {{signal("component.a"), occurrence("a"), 1U, owner("a")},
           {signal("component.a", "alias"), occurrence("a"), 1U, owner("a")},
           {signal("component.b"), occurrence("b"), 1U, owner("b")}},
          core::ActiveBundlePublicationGeneration(1U)};
}

class MockBooleanOutputProviderTest : public ::testing::Test {
protected:
  void Build() {
    auto created = MockBooleanOutputProvider::Create(clock, channel());
    ASSERT_TRUE(created.ok()) << created.error().message;
    provider = std::move(created.value());
  }
  bool Write(bool value, std::string component = "component.a",
             std::string name = "output") {
    return provider->WriteOutput(signal(std::move(component), std::move(name)), value)
               .outcome == core::ActionOutcome::kSucceeded;
  }
  core::FakeClock clock;
  std::unique_ptr<MockBooleanOutputProvider> provider;
};

TEST_F(MockBooleanOutputProviderTest,
       SharedChannelAliasesKeepDistinctActualDomainEnableFences) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(Write(true, "component.a", "alias"));
  EXPECT_FALSE(Write(false, "component.b"));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2U, owner("b")).ok());
  EXPECT_TRUE(Write(false, "component.b"));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_FALSE(Write(true));
  EXPECT_TRUE(Write(true, "component.b"));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
}

TEST_F(MockBooleanOutputProviderTest,
       DisableEnablePreserveModelAndStaleDriverTransitionsCannotReopen) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(Write(true));
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_FALSE(Write(false));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  EXPECT_FALSE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3U, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  EXPECT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  EXPECT_TRUE(Write(false));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
}

TEST_F(MockBooleanOutputProviderTest, ForeignOccurrenceCannotOpenOrCloseTheGate) {
  Build();
  auto stale = occurrence("a");
  stale.provider_runtime_epoch = core::RuntimeEpoch("runtime.old");
  EXPECT_FALSE(provider->Resume(stale, 99U, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(stale, 99U, owner("a")).ok());
  EXPECT_FALSE(Write(true));
  EXPECT_FALSE(provider->model_value().has_value());
}

TEST_F(MockBooleanOutputProviderTest,
       ShutdownFencesNewWritesWhileReplacementStartsUnwritten) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(Write(true));
  provider->Shutdown();
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_FALSE(Write(false));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  const auto replacement = MockBooleanOutputProvider::Create(clock, channel());
  ASSERT_TRUE(replacement.ok());
  EXPECT_FALSE(replacement.value()->model_value().has_value());
}

TEST_F(MockBooleanOutputProviderTest,
       TerminalOccurrenceFenceAtMaximumGenerationPreservesOtherAlias) {
  Build();
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  ASSERT_TRUE(provider->Resume(occurrence("a"), maximum, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("b"), maximum, owner("b")).ok());
  ASSERT_TRUE(Write(true));
  ASSERT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_FALSE(
      provider->ShutdownOccurrence(occurrence("foreign"), owner("foreign"))
          .ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), maximum, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), maximum, owner("a")).ok());
  EXPECT_FALSE(Write(false, "component.a", "alias"));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  EXPECT_TRUE(Write(false, "component.b"));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
}

TEST_F(MockBooleanOutputProviderTest,
       NewApplyResetsOnlyModelAndExactReplayPreservesLaterWrites) {
  Build();
  const auto before = provider->ReadAliasState();
  EXPECT_EQ(before.active_publication_generation,
            core::ActiveBundlePublicationGeneration(1U));
  ASSERT_EQ(before.aliases.size(), 3U);
  EXPECT_TRUE(SameIoOutputDriverOwner(
      before.aliases.front().actual_driver_owner, owner("a")));
  EXPECT_TRUE(std::is_sorted(
      before.aliases.begin(), before.aliases.end(),
      [](const auto &a, const auto &b) { return a.signal_id < b.signal_id; }));
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(Write(true));
  const auto predecessor = channel().aliases;
  auto candidate = predecessor;
  const auto added_owner = std::make_shared<const int>(4);
  candidate.push_back(
      {signal("component.c"), occurrence("c"), 1U, added_owner});
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(predecessor), candidate,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  const auto after = provider->ReadAliasState();
  EXPECT_EQ(after.active_publication_generation,
            core::ActiveBundlePublicationGeneration(2U));
  ASSERT_EQ(after.aliases.size(), 4U);
  EXPECT_TRUE(SameIoOutputDriverOwner(after.aliases.back().actual_driver_owner,
                                      added_owner));
  EXPECT_EQ(before.aliases.size(),
            3U); // A returned snapshot cannot be retargeted.

  EXPECT_FALSE(provider->model_value());
  ASSERT_TRUE(Write(false));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  // Same actual Apply notice is a no-op even after a genuine later write.
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(predecessor), candidate,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  EXPECT_FALSE(Write(true, "component.c"));
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  ASSERT_TRUE(provider->Resume(occurrence("c"), 2U, added_owner).ok());
  EXPECT_TRUE(Write(true, "component.c"));
}

TEST_F(MockBooleanOutputProviderTest,
       EqualOccurrenceNewOwnerCannotBeFencedByRetiredOwner) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2U, owner("b")).ok());
  ASSERT_TRUE(Write(true));
  ASSERT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  auto current = channel().aliases;
  auto removed = current;
  std::erase_if(
      removed, [](const auto &row) { return row.provider == occurrence("a"); });
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(current), removed,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  const auto new_owner = std::make_shared<const int>(5);
  for (auto &alias : current)
    if (alias.provider == occurrence("a"))
      alias.actual_driver_owner = new_owner;
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(removed), current,
                                  core::ActiveBundlePublicationGeneration(3U))
                  .ok());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, new_owner).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), 99U, owner("a")).ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 99U, owner("a")).ok());
  EXPECT_FALSE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_TRUE(Write(false, "component.b"));
  EXPECT_TRUE(Write(true));
}

TEST_F(MockBooleanOutputProviderTest,
       AliasReplacementRefusesStaleOrOwnerlessProofWithoutReset) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(Write(true));
  auto candidate = channel().aliases;
  candidate.push_back(
      {signal("component.a", "new_alias"), occurrence("a"), 1U, owner("a")});
  EXPECT_FALSE(provider
                   ->RebindAliases(identities(channel().aliases), candidate,
                                   core::ActiveBundlePublicationGeneration(1U))
                   .ok());
  EXPECT_FALSE(provider
                   ->RebindAliases({}, candidate,
                                   core::ActiveBundlePublicationGeneration(2U))
                   .ok());
  EXPECT_FALSE(provider
                   ->RebindAliases(identities(channel().aliases), candidate,
                                   core::ActiveBundlePublicationGeneration(0U))
                   .ok());
  candidate.back().actual_driver_owner.reset();
  EXPECT_FALSE(provider
                   ->RebindAliases(identities(channel().aliases), candidate,
                                   core::ActiveBundlePublicationGeneration(2U))
                   .ok());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(channel().aliases),
                                  channel().aliases,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 99U, owner("a")).ok());
  EXPECT_FALSE(provider
                   ->RebindAliases(identities(channel().aliases),
                                   channel().aliases,
                                   core::ActiveBundlePublicationGeneration(1U))
                   .ok());
}

TEST_F(MockBooleanOutputProviderTest, ExpiredActualOwnerCannotSendOrResume) {
  auto configuration = channel();
  auto actual_owner = std::make_shared<const int>(6);
  for (auto &alias : configuration.aliases)
    if (alias.provider == occurrence("a"))
      alias.actual_driver_owner = actual_owner;
  auto created = MockBooleanOutputProvider::Create(clock, configuration);
  ASSERT_TRUE(created.ok());
  provider = std::move(created.value());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, actual_owner).ok());
  const std::weak_ptr<const void> weak = actual_owner;
  actual_owner.reset();
  ASSERT_TRUE(weak.expired());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3U, weak).ok());
  EXPECT_FALSE(Write(true));
  EXPECT_FALSE(provider->model_value());
  // Terminal teardown still identifies the exact expired control block.
  EXPECT_TRUE(provider->ShutdownOccurrence(occurrence("a"), weak).ok());
}

TEST(MockBooleanOutputConfigurationTest,
     InvalidAliasGroupsDoNotCreateIndependentOrAmbiguousChannelState) {
  core::FakeClock clock;
  auto config = channel();
  config.aliases.push_back(config.aliases.front());
  EXPECT_FALSE(MockBooleanOutputProvider::Create(clock, config).ok());
  config = channel();
  config.aliases[1].prepared_driver_generation = 2U;
  EXPECT_FALSE(MockBooleanOutputProvider::Create(clock, config).ok());
  config = channel();
  config.aliases.front().prepared_driver_generation = 0U;
  EXPECT_FALSE(MockBooleanOutputProvider::Create(clock, config).ok());
  config = channel();
  config.channel = "not portable";
  EXPECT_FALSE(MockBooleanOutputProvider::Create(clock, config).ok());
}

// The one write of one exact alias: refused
// before anything is written while its occurrence is disabled, then taken by
// the model at once with the same echo; another component's alias of the
// same name stays its own.
TEST_F(MockBooleanOutputProviderTest, ManualWriteTakesTheValueOnlyWhileItsOccurrenceIsEnabled) {
  Build();
  std::vector<bool> echoed;
  provider->SetModelObserver([&](const bool value, core::TimePoint) { echoed.push_back(value); });
  const auto disabled = provider->WriteOutput(signal("component.a"), true);
  EXPECT_EQ(disabled.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(disabled.error.has_value());
  EXPECT_EQ(disabled.error->code, "io_output_not_submitted");
  EXPECT_FALSE(provider->model_value().has_value());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  const auto written = provider->WriteOutput(signal("component.a"), true);
  EXPECT_EQ(written.outcome, core::ActionOutcome::kSucceeded);
  EXPECT_FALSE(written.error.has_value());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  EXPECT_EQ(echoed, std::vector<bool>{true});
  // component.b's alias of the same name has its own, disabled occurrence.
  EXPECT_EQ(provider->WriteOutput(signal("component.b"), false).error->code,
            "io_output_not_submitted");
  EXPECT_EQ(provider->WriteOutput(signal("component.a", "absent"), false).error->code,
            "io_signal_unknown");
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
}

} // namespace
} // namespace manyforge::roboplan_backend
