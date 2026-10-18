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

core::IoOutputWriteRequestDTO request(std::uint64_t number, bool value = true,
                                      std::string component = "component.a",
                                      std::string provider = "a") {
  return {{{core::OperationId("operation." + std::to_string(number)),
            core::OperationRevision(1U),
            core::OperationParticipantId("participant"),
            core::ControllerGoalId("goal." + std::to_string(number)),
            core::ControllerGoalGeneration(1U)},
           kDigest,
           core::ComponentLifecycleGeneration(7U),
           occurrence(std::move(provider))},
          signal(std::move(component)),
          value,
          std::nullopt};
}

class MockBooleanOutputProviderTest : public ::testing::Test {
protected:
  void Build() {
    auto created = MockBooleanOutputProvider::Create(clock, channel());
    ASSERT_TRUE(created.ok()) << created.error().message;
    provider = std::move(created.value());
  }
  core::FakeClock clock;
  std::unique_ptr<MockBooleanOutputProvider> provider;
};

TEST_F(MockBooleanOutputProviderTest,
       StartsUnwrittenAndDisabledRefusalIsRetainedWithoutModelWrite) {
  Build();
  EXPECT_FALSE(provider->model_value().has_value());
  EXPECT_EQ(provider->StrongestOutputEvidence(),
            core::IoOutputEvidence::kSourceAccepted);
  const auto first = request(1U);
  const auto disabled = provider->SubmitOutput(first);
  ASSERT_TRUE(disabled.ok());
  EXPECT_EQ(disabled.value().identity, first.identity);
  EXPECT_EQ(disabled.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(disabled.value().error.has_value());
  EXPECT_FALSE(provider->model_value().has_value());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  EXPECT_EQ(provider->SubmitOutput(first).value(), disabled.value());
  EXPECT_FALSE(provider->model_value().has_value());
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  const auto accepted = provider->SubmitOutput(request(2U));
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value().evidence, core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
}

TEST_F(MockBooleanOutputProviderTest,
       ImmediateAcceptanceReplayAndObservationNeedNoDeadline) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  clock.advance(10);
  auto write = request(1U, false);
  EXPECT_FALSE(write.absolute_deadline_ns);
  const auto accepted = provider->SubmitOutput(write);
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value().trusted_received_at_ns, 10);
  EXPECT_EQ(accepted.value().evidence, core::IoOutputEvidence::kSourceAccepted);
  EXPECT_FALSE(accepted.value().error.has_value());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  clock.advance(30);
  EXPECT_EQ(provider->SubmitOutput(write).value(), accepted.value());
  EXPECT_EQ(provider->ObserveOutputResult(write.identity).value(),
            accepted.value());
  ASSERT_TRUE(provider->RetireOutput(write.identity).ok());
  auto timed = request(2U, true);
  timed.absolute_deadline_ns = clock.now() + 100;
  const auto refused = provider->SubmitOutput(timed);
  ASSERT_TRUE(refused.ok());
  EXPECT_EQ(refused.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(refused.value().error.has_value());
  EXPECT_EQ(refused.value().error->details.at("reason"),
            "io_output_completion_policy_mismatch");
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  clock.advance(1000);
  EXPECT_EQ(provider->SubmitOutput(timed).value(), refused.value());
  ASSERT_TRUE(provider->RetireOutput(timed.identity).ok());
  const auto next = provider->SubmitOutput(request(3U, true));
  ASSERT_TRUE(next.ok());
  EXPECT_EQ(next.value().evidence, core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
}

TEST_F(MockBooleanOutputProviderTest,
       ActiveCorrelationCannotBeChangedEvictedOrRetiredByAnotherIdentity) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  const auto write = request(1U, false);
  const auto accepted = provider->SubmitOutput(write);
  ASSERT_TRUE(accepted.ok());
  auto changed = write;
  changed.value = true;
  EXPECT_FALSE(provider->SubmitOutput(changed).ok());
  changed = write;
  changed.absolute_deadline_ns = 1;
  EXPECT_FALSE(provider->SubmitOutput(changed).ok());
  changed = write;
  changed.signal_id = signal("component.a", "alias");
  EXPECT_FALSE(provider->SubmitOutput(changed).ok());
  EXPECT_FALSE(provider->SubmitOutput(request(2U)).ok());
  EXPECT_FALSE(provider->RetireOutput(request(2U).identity).ok());
  EXPECT_EQ(provider->ObserveOutputResult(write.identity).value(),
            accepted.value());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  ASSERT_TRUE(provider->RetireOutput(write.identity).ok());
  EXPECT_FALSE(provider->ObserveOutputResult(write.identity).ok());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  EXPECT_EQ(provider->SubmitOutput(request(2U)).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
}

TEST_F(MockBooleanOutputProviderTest,
       SharedChannelAliasesKeepDistinctActualDomainEnableFences) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  auto first = request(1U);
  first.signal_id = signal("component.a", "alias");
  ASSERT_EQ(provider->SubmitOutput(first).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  const auto disabled = request(2U, false, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(disabled).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2U, owner("b")).ok());
  EXPECT_EQ(provider->SubmitOutput(disabled).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  ASSERT_TRUE(provider->RetireOutput(disabled.identity).ok());
  const auto second = request(3U, false, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(second).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  ASSERT_TRUE(provider->RetireOutput(second.identity).ok());
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  const auto third = request(4U, true, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(third).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
}

TEST_F(MockBooleanOutputProviderTest,
       DisableEnablePreserveModelAndStaleDriverTransitionsCannotReopen) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  const auto first = request(1U);
  const auto accepted = provider->SubmitOutput(first);
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_EQ(provider->ObserveOutputResult(first.identity).value(),
            accepted.value());
  EXPECT_EQ(provider->SubmitOutput(first).value(), accepted.value());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  EXPECT_FALSE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3U, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  EXPECT_TRUE(provider->Resume(occurrence("a"), 4U, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  EXPECT_EQ(provider->SubmitOutput(request(2U, false)).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
}

TEST_F(MockBooleanOutputProviderTest,
       ForeignOccurrenceAndMalformedIdentityNeverClaimAcceptance) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  auto write = request(1U);
  write.identity.provider.provider_runtime_epoch =
      core::RuntimeEpoch("runtime.old");
  EXPECT_FALSE(provider->SubmitOutput(write).ok());
  auto stale = occurrence("a");
  stale.provider_runtime_epoch = core::RuntimeEpoch("runtime.old");
  EXPECT_FALSE(provider->Resume(stale, 99U, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(stale, 99U, owner("a")).ok());
  write = request(1U);
  write.identity.command.command_generation =
      core::ControllerGoalGeneration(0U);
  EXPECT_FALSE(provider->SubmitOutput(write).ok());
  write = request(1U);
  write.identity.prepared_contract_digest = core::ContentDigest("invalid");
  EXPECT_FALSE(provider->SubmitOutput(write).ok());
  write = request(1U);
  write.signal_id = signal("missing");
  EXPECT_FALSE(provider->SubmitOutput(write).ok());
  EXPECT_FALSE(provider->model_value().has_value());
  EXPECT_EQ(provider->SubmitOutput(request(1U)).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
}

TEST_F(MockBooleanOutputProviderTest,
       ShutdownFencesNewWritesWhileReplacementStartsUnwritten) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  const auto first = request(1U);
  const auto accepted = provider->SubmitOutput(first);
  ASSERT_TRUE(accepted.ok());
  provider->Shutdown();
  EXPECT_FALSE(provider->Resume(occurrence("a"), 3U, owner("a")).ok());
  EXPECT_EQ(provider->ObserveOutputResult(first.identity).value(),
            accepted.value());
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  EXPECT_EQ(provider->SubmitOutput(request(2U, false)).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  const auto replacement = MockBooleanOutputProvider::Create(clock, channel());
  ASSERT_TRUE(replacement.ok());
  EXPECT_FALSE(replacement.value()->model_value().has_value());
}

TEST_F(
    MockBooleanOutputProviderTest,
    TerminalOccurrenceFenceAtMaximumGenerationPreservesOtherAliasAndEvidence) {
  Build();
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  ASSERT_TRUE(provider->Resume(occurrence("a"), maximum, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("b"), maximum, owner("b")).ok());
  const auto first = request(1U, true);
  const auto accepted = provider->SubmitOutput(first);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(accepted.value().evidence, core::IoOutputEvidence::kSourceAccepted);
  ASSERT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_TRUE(provider->ShutdownOccurrence(occurrence("a"), owner("a")).ok());
  EXPECT_FALSE(
      provider->ShutdownOccurrence(occurrence("foreign"), owner("foreign"))
          .ok());
  EXPECT_FALSE(provider->Resume(occurrence("a"), maximum, owner("a")).ok());
  EXPECT_FALSE(provider->Pause(occurrence("a"), maximum, owner("a")).ok());
  EXPECT_EQ(provider->SubmitOutput(first).value(), accepted.value());
  EXPECT_EQ(provider->ObserveOutputResult(first.identity).value(),
            accepted.value());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
  auto closed = request(2U, false);
  closed.signal_id = signal("component.a", "alias");
  const auto refused = provider->SubmitOutput(closed);
  ASSERT_TRUE(refused.ok());
  EXPECT_EQ(refused.value().evidence, core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(true));
  ASSERT_TRUE(provider->RetireOutput(closed.identity).ok());
  const auto other = request(3U, false, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(other).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
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
  auto first = request(1U);
  ASSERT_EQ(provider->SubmitOutput(first).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
  ASSERT_TRUE(provider->RetireOutput(first.identity).ok());
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
  EXPECT_EQ(before.active_publication_generation,
            core::ActiveBundlePublicationGeneration(1U));
  EXPECT_EQ(before.aliases.size(),
            3U); // A returned snapshot cannot be retargeted.

  EXPECT_FALSE(provider->model_value());
  auto retained = request(2U, false);
  ASSERT_EQ(provider->SubmitOutput(retained).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  // Same actual Apply notice is a no-op even after a genuine later write.
  ASSERT_TRUE(provider
                  ->RebindAliases(identities(predecessor), candidate,
                                  core::ActiveBundlePublicationGeneration(2U))
                  .ok());
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  ASSERT_TRUE(provider->RetireOutput(retained.identity).ok());
  const auto added = request(3U, true, "component.c", "c");
  EXPECT_EQ(provider->SubmitOutput(added).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
  EXPECT_EQ(provider->model_value(), std::optional<bool>(false));
  ASSERT_TRUE(provider->RetireOutput(added.identity).ok());
  ASSERT_TRUE(provider->Resume(occurrence("c"), 2U, added_owner).ok());
  EXPECT_EQ(provider->SubmitOutput(request(4U, true, "component.c", "c"))
                .value()
                .evidence,
            core::IoOutputEvidence::kSourceAccepted);
}

TEST_F(MockBooleanOutputProviderTest,
       EqualOccurrenceNewOwnerCannotBeFencedByRetiredOwner) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  ASSERT_TRUE(provider->Resume(occurrence("b"), 2U, owner("b")).ok());
  const auto old = request(1U);
  const auto accepted = provider->SubmitOutput(old).value();
  ASSERT_EQ(accepted.evidence, core::IoOutputEvidence::kSourceAccepted);
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
  EXPECT_EQ(provider->ObserveOutputResult(old.identity).value(), accepted);
  EXPECT_EQ(provider->SubmitOutput(old).value(), accepted);
  EXPECT_FALSE(provider->SubmitOutput(request(2U)).ok());
  ASSERT_TRUE(provider->RetireOutput(old.identity).ok());
  auto other = request(3U, false, "component.b", "b");
  EXPECT_EQ(provider->SubmitOutput(other).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
  ASSERT_TRUE(provider->RetireOutput(other.identity).ok());
  EXPECT_EQ(provider->SubmitOutput(request(4U)).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
}

TEST_F(MockBooleanOutputProviderTest,
       AliasReplacementRefusesStaleOrOwnerlessProofWithoutReset) {
  Build();
  ASSERT_TRUE(provider->Resume(occurrence("a"), 2U, owner("a")).ok());
  const auto write = request(1U);
  ASSERT_EQ(provider->SubmitOutput(write).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
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
  EXPECT_EQ(provider->ObserveOutputResult(write.identity).value().evidence,
            core::IoOutputEvidence::kSourceAccepted);
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
  EXPECT_EQ(provider->SubmitOutput(request(1U)).value().evidence,
            core::IoOutputEvidence::kNotSubmitted);
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

} // namespace
} // namespace manyforge::roboplan_backend
