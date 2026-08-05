#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "manyforge_headless/source_snapshot.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-snapshot-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("source snapshot test mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

CanonicalValue Payload(std::string value) {
  return CanonicalValue(CanonicalValue::Object{
      {"available", CanonicalValue(true)},
      {"value", CanonicalValue(std::move(value))},
  });
}

TEST(SourceSnapshotCodec, RoundTripsCanonicalClosedRecordAndRejectsTamper) {
  const auto snapshot =
      SourceSnapshotCodec::Create("component_manager", "manager.cell-a",
                                  "epoch-a", 7U, 1000, Payload("alpha"));
  ASSERT_TRUE(snapshot.valid());
  const auto bytes = SourceSnapshotCodec::Encode(snapshot);
  EXPECT_EQ(SourceSnapshotCodec::Decode(bytes), snapshot);

  auto tampered = bytes;
  const auto position = tampered.find("alpha");
  ASSERT_NE(position, std::string::npos);
  tampered.replace(position, 5U, "bravo");
  EXPECT_THROW(SourceSnapshotCodec::Decode(tampered),
               SourceSnapshotIntegrityError);

  auto noncanonical = bytes;
  noncanonical.push_back('\n');
  EXPECT_THROW(SourceSnapshotCodec::Decode(noncanonical),
               SourceSnapshotIntegrityError);
}

TEST(AtomicSourceSnapshotChannel,
     SkipsCrashLeftoverTemporaryNameWithoutDeletingIt) {
  TemporaryDirectory directory;
  const auto leftover = directory.path() / (".manager.json.tmp." +
                                            std::to_string(::getpid()) + ".0");
  {
    std::ofstream stream(leftover);
    stream << "crash-leftover";
  }

  AtomicSourceSnapshotChannel channel(directory.path(), "manager.json");
  const auto published = channel.Publish("component_manager", "manager.cell-a",
                                         "epoch-a", 1000, Payload("alpha"));

  EXPECT_EQ(published.source_generation, 1U);
  EXPECT_TRUE(std::filesystem::exists(leftover));
  ASSERT_TRUE(channel.Read().has_value());
  EXPECT_EQ(*channel.Read(), published);
}

TEST(AtomicSourceSnapshotChannel,
     GenerationChangesOnlyForSemanticsWithinOneOwnerEpoch) {
  TemporaryDirectory directory;
  AtomicSourceSnapshotChannel channel(directory.path(), "manager.json");
  EXPECT_FALSE(channel.Read().has_value());

  const auto first = channel.Publish("component_manager", "manager.cell-a",
                                     "epoch-a", 1000, Payload("alpha"));
  EXPECT_EQ(first.source_generation, 1U);
  const auto observed_first = channel.ReadObserved();
  ASSERT_TRUE(observed_first.has_value());
  EXPECT_EQ(observed_first->snapshot, first);
  EXPECT_GT(observed_first->file_modified_at_unix_ns, 0);
  const auto freshness_only = channel.Publish(
      "component_manager", "manager.cell-a", "epoch-a", 2000, Payload("alpha"));
  EXPECT_EQ(freshness_only.source_generation, 1U);
  EXPECT_EQ(freshness_only.semantic_content_digest,
            first.semantic_content_digest);
  EXPECT_NE(freshness_only.record_digest, first.record_digest);

  const auto changed = channel.Publish("component_manager", "manager.cell-a",
                                       "epoch-a", 3000, Payload("bravo"));
  EXPECT_EQ(changed.source_generation, 2U);
  EXPECT_NE(changed.semantic_content_digest,
            freshness_only.semantic_content_digest);

  AtomicSourceSnapshotChannel reopened(directory.path(), "manager.json");
  ASSERT_TRUE(reopened.Read().has_value());
  EXPECT_EQ(*reopened.Read(), changed);
  const auto replacement = reopened.Publish(
      "component_manager", "manager.cell-a", "epoch-b", 4000, Payload("bravo"));
  EXPECT_EQ(replacement.source_generation, 1U);
  EXPECT_NE(replacement.semantic_content_digest,
            changed.semantic_content_digest);

  EXPECT_THROW(reopened.Publish("logical_scene", "scene.cell-a", "epoch-b",
                                5000, Payload("bravo")),
               SourceSnapshotIntegrityError);
}

TEST(AtomicSourceSnapshotChannel,
     ConcurrentReadersObserveOnlyCompleteOldOrNewRecords) {
  TemporaryDirectory directory;
  AtomicSourceSnapshotChannel writer(directory.path(), "manager.json");
  static_cast<void>(writer.Publish("component_manager", "manager.cell-a",
                                   "epoch-a", 1, Payload("initial")));

  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      AtomicSourceSnapshotChannel reader(directory.path(), "manager.json");
      while (!stop.load()) {
        try {
          const auto snapshot = reader.Read();
          if (!snapshot.has_value() || !snapshot->valid()) {
            failed.store(true);
            return;
          }
        } catch (...) {
          failed.store(true);
          return;
        }
      }
    });
  }

  for (std::int64_t generation = 2; generation <= 100; ++generation) {
    static_cast<void>(writer.Publish(
        "component_manager", "manager.cell-a", "epoch-a", generation,
        Payload("value-" + std::to_string(generation))));
  }
  stop.store(true);
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_FALSE(failed.load());
  const auto final = writer.Read();
  ASSERT_TRUE(final.has_value());
  EXPECT_EQ(final->source_generation, 100U);
}

TEST(AtomicSourceSnapshotChannel, RejectsSymlinkPublicationAndIgnoresTemps) {
  TemporaryDirectory directory;
  {
    std::ofstream temporary(directory.path() / ".manager.json.tmp.stale");
    temporary << "not a publication";
  }
  AtomicSourceSnapshotChannel channel(directory.path(), "manager.json");
  EXPECT_FALSE(channel.Read().has_value());

  const auto target = directory.path() / "target.json";
  {
    std::ofstream stream(target);
    stream << "{}";
  }
  std::filesystem::create_symlink(target.filename(),
                                  directory.path() / "manager.json");
  EXPECT_THROW(channel.Read(), SourceSnapshotIoError);
  EXPECT_THROW(channel.Publish("component_manager", "manager.cell-a", "epoch-a",
                               1, Payload("alpha")),
               SourceSnapshotIoError);
}

} // namespace
} // namespace manyforge::headless
