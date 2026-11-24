#pragma once

// The Manager's status stream (OD-216): its state reaches the Headless API
// from memory over a kept local connection, never through files.
//
// One private SEQPACKET socket of its own (`manager-status.sock` in the
// Manager run root), accepted by its own thread, one writer thread per
// subscriber (at most 4). The main loop never sees a subscriber: producers
// (main loop, Apply lane) only swap the newest value of a section into the
// board, and the loop advances the board's tick every 500 ms. A slow or dead
// reader only holds its own writer.
//
// Manager -> client, one packet per frame, RFC 8785 canonical JSON, each
//   {"api_version": "manyforge.manager_status_stream.v1",
//    "application_start_id": <A>, "tick": <u64>, "type": ...}:
//   snapshot  {generations: {logical_scene, manager, scene_runtime,
//             viewer_pose_sessions}}  first frame; then one `section` frame
//             per section with generation >= 1, each at or above the listed
//             generation.
//   section   {name, section: {generation, observed_at_ns, oversized,
//             payload}}  a section's whole newest value, sent when its
//             generation differs from the last one sent (newest wins; values
//             in between are skipped). `oversized` true (payload null): the
//             value exceeds this connection's frame bound.
//   keepalive {}  the tick advanced and nothing else was sent this round.
//   error     {error}  the subscription ends (`manager_status_subscribers_full`).
// `tick` counts the main loop's status ticks of this start (from 1 at its
// first loop pass); a frame carrying a higher tick than the last one proves
// the loop ran since. Sections: `manager` (`manyforge.manager_source.v2`),
// `logical_scene` (`manyforge.logical_scene_source.v1`), `scene_runtime`
// (`manyforge.scene_runtime_source.v1`), `viewer_pose_sessions`
// (`{runtime_identity, sessions}`, fixed by this stream's version). A
// section's generation starts at 1 in this start and rises only when its
// payload bytes change; `observed_at_ns` is the producer's time of that value.
// `logical_scene` and `scene_runtime` have concurrent producers: each value
// carries the number of the read it was built from, and the board keeps the
// value of the highest number (OD-302), so a section never goes back.

#include "manager_response_transport_internal.hpp"
#include "manyforge_headless/source_contract.hpp"

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_private_stream/private_stream_server.hpp>
#include <manyforge_scene/logical_scene_owner.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::headless {

inline constexpr std::string_view kManagerStatusSocketFileName =
    "manager-status.sock";

enum class ManagerStatusSection : std::size_t {
  kManager = 0U,
  kLogicalScene = 1U,
  kSceneRuntime = 2U,
  kViewerPoseSessions = 3U,
};
inline constexpr std::size_t kManagerStatusSectionCount = 4U;
/// Section names in the fixed order the writer sends them.
inline constexpr std::array<std::string_view, kManagerStatusSectionCount>
    kManagerStatusSectionNames{"manager", "logical_scene", "scene_runtime",
                               "viewer_pose_sessions"};

/// The newest value of every section and the main loop's tick. Producers
/// publish from any thread, also under their own lock: `Publish` is memory
/// work on one atomic view, with no lock. The writers are woken by
/// `NotifyIfChanged`, which takes the change signal's short lock and is
/// therefore called with no other lock held.
class ManagerStatusBoard final {
public:
  struct Section final {
    std::uint64_t generation{};
    std::int64_t observed_at_ns{};
    /// Canonical JSON of the payload; null while generation is 0.
    std::shared_ptr<const std::string> payload;
    /// The order key of the held value (PublishOrdered); 0 for a section
    /// published without one.
    std::uint64_t order{};
  };
  struct View final {
    std::uint64_t tick{};
    std::array<Section, kManagerStatusSectionCount> sections{};
  };

  explicit ManagerStatusBoard(core::ExecutionEventChangeSignal &signal)
      : signal_(signal), view_(std::make_shared<const View>()) {}

  ManagerStatusBoard(const ManagerStatusBoard &) = delete;
  ManagerStatusBoard &operator=(const ManagerStatusBoard &) = delete;

  /// Stores `payload` as the section's newest value and returns its
  /// generation, which rises only when the encoded bytes differ (an
  /// unchanged value keeps its generation and its `observed_at_ns`).
  /// For a section whose producers are serialized (`manager`,
  /// `viewer_pose_sessions`).
  std::uint64_t Publish(const ManagerStatusSection section,
                        const std::int64_t observed_at_ns,
                        const content_identity::CanonicalValue &payload) {
    return Store(section, std::nullopt, observed_at_ns, payload);
  }

  /// For a section with concurrent producers (`logical_scene`,
  /// `scene_runtime`, OD-302): `order` is the key of the read the payload
  /// was built from, rising with the state read. Stores like Publish only
  /// when `order` is above the held value's key, decided in the same swap;
  /// an older or equal key changes nothing and returns the held value's
  /// generation, whose value is at least as new. A newer key with unchanged
  /// bytes keeps the generation and time and takes the newer key.
  std::uint64_t PublishOrdered(const ManagerStatusSection section,
                               const std::uint64_t order,
                               const std::int64_t observed_at_ns,
                               const content_identity::CanonicalValue &payload) {
    return Store(section, order, observed_at_ns, payload);
  }

  /// One status tick of the main loop (only the loop calls it).
  void AdvanceTick() {
    auto current = view_.load();
    for (;;) {
      auto next = std::make_shared<View>(*current);
      ++next->tick;
      if (view_.compare_exchange_weak(current,
                                      std::shared_ptr<const View>(std::move(next)))) {
        changed_.store(true, std::memory_order_release);
        return;
      }
    }
  }

  /// Wakes the writers once after one or more changes. Called with no lock
  /// held.
  void NotifyIfChanged() noexcept {
    if (changed_.exchange(false, std::memory_order_acq_rel))
      signal_.Notify();
  }

  [[nodiscard]] std::shared_ptr<const View> Load() const noexcept {
    return view_.load();
  }

  [[nodiscard]] core::ExecutionEventChangeSignal &signal() const noexcept {
    return signal_;
  }

private:
  std::uint64_t Store(const ManagerStatusSection section,
                      const std::optional<std::uint64_t> order,
                      const std::int64_t observed_at_ns,
                      const content_identity::CanonicalValue &payload) {
    auto bytes = std::make_shared<const std::string>(
        content_identity::canonical_json(payload));
    const auto index = static_cast<std::size_t>(section);
    auto current = view_.load();
    for (;;) {
      const auto &old = current->sections[index];
      if (order.has_value() && *order <= old.order)
        return old.generation;
      const bool unchanged = old.payload != nullptr && *old.payload == *bytes;
      if (unchanged && !order.has_value())
        return old.generation;
      auto next = std::make_shared<View>(*current);
      auto &slot = next->sections[index];
      if (unchanged)
        slot.order = *order;
      else
        slot = Section{old.generation + 1U, observed_at_ns, bytes,
                       order.value_or(0U)};
      const auto generation = slot.generation;
      if (view_.compare_exchange_weak(current,
                                      std::shared_ptr<const View>(std::move(next)))) {
        if (!unchanged)
          changed_.store(true, std::memory_order_release);
        return generation;
      }
    }
  }

  core::ExecutionEventChangeSignal &signal_;
  std::atomic<std::shared_ptr<const View>> view_;
  std::atomic<bool> changed_{false};
};

/// The acceptor and writers of the status stream.
class ManagerStatusStream final {
public:
  static constexpr std::size_t kMaximumSubscribers = 4U;
  /// The largest frame: the largest scene the owner commits plus 16 KiB for
  /// the section's access part, the frame envelope and the few bytes per
  /// object the section's encoding can exceed the owner's (at most about
  /// 2 KiB in all), so a committed scene always fits one `logical_scene`
  /// section frame. A connection whose send
  /// buffer cannot hold it uses its own smaller capacity.
  static constexpr std::size_t kMaximumFrameBytes =
      manyforge::scene::kMaximumLogicalSceneSnapshotBytes + 16U * 1024U;
  /// A reader that accepts no frame for this long is dropped; by then it has
  /// already declared the Manager not answering (its 2 s rule).
  static constexpr std::chrono::milliseconds kSendDeadline{2000};

  ManagerStatusStream(const ManagerStatusBoard &board,
                      std::string application_start_id)
      : board_(board), application_start_id_(std::move(application_start_id)) {}

  ManagerStatusStream(const ManagerStatusStream &) = delete;
  ManagerStatusStream &operator=(const ManagerStatusStream &) = delete;

  /// Accepts subscribers on the listening `socket` until `stop` (its own
  /// thread), then ends every subscription and returns once all are closed.
  void Run(const int socket, const std::stop_token stop) const noexcept {
    private_stream::AcceptSubscribers(
        socket,
        private_stream::SubscriberPoolOptions{
            kMaximumSubscribers, std::string(kManagerStatusStreamApiVersion),
            "manager_status_subscribers_full",
            "the Manager serves its bounded number of status subscriptions",
            kMaximumFrameBytes, kSendDeadline},
        [this](const int subscriber, const std::stop_token writer_stop) {
          Serve(subscriber, writer_stop);
        },
        stop);
  }

  /// Serves one subscriber until it fails, disconnects, is too slow, or
  /// `stop`. Reads only the board; never holds a lock while it sends.
  void Serve(const int client, const std::stop_token stop) const noexcept {
    try {
      const auto capacity =
          detail::AtomicResponseCapacity(client, kMaximumFrameBytes);
      if (capacity == 0U)
        return;
      auto &signal = board_.signal();
      auto generation = signal.Generation();
      auto view = board_.Load();
      if (!Send(client, Snapshot(*view), capacity, stop))
        return;
      std::array<std::uint64_t, kManagerStatusSectionCount> sent{};
      std::uint64_t sent_tick = view->tick;
      for (;;) {
        bool sent_any = false;
        for (std::size_t index = 0U; index < kManagerStatusSectionCount; ++index) {
          const auto &section = view->sections[index];
          if (section.generation == sent[index])
            continue;
          if (!Send(client, SectionFrame(*view, index, capacity), capacity, stop))
            return;
          sent[index] = section.generation;
          sent_any = true;
        }
        if (!sent_any && view->tick > sent_tick &&
            !Send(client, Envelope(view->tick, "keepalive", ""), capacity, stop))
          return;
        sent_tick = view->tick;
        // The loop notifies at every status tick, so this wait has no
        // deadline of its own; the hour only bounds one wait call.
        signal.WaitAfter(generation,
                         std::chrono::steady_clock::now() + std::chrono::hours(1),
                         stop);
        if (stop.stop_requested())
          return;
        generation = signal.Generation();
        view = board_.Load();
      }
    } catch (...) {
      // Encoding a frame can only fail on memory; the subscription ends and
      // the reader reconnects.
    }
  }

private:
  [[nodiscard]] static std::string Text(const std::string_view value) {
    return content_identity::canonical_json(
        content_identity::CanonicalValue(std::string(value)));
  }

  [[nodiscard]] static std::string Number(const std::uint64_t value) {
    return content_identity::canonical_json(
        content_identity::CanonicalValue(static_cast<double>(value)));
  }

  /// `{"api_version", "application_start_id", <members>, "tick", "type"}`
  /// in canonical member order: every frame's own members sort between
  /// `application_start_id` and `tick`.
  [[nodiscard]] std::string Envelope(const std::uint64_t tick,
                                     const std::string_view type,
                                     const std::string_view members) const {
    std::string frame = "{\"api_version\":";
    frame += Text(kManagerStatusStreamApiVersion);
    frame += ",\"application_start_id\":";
    frame += Text(application_start_id_);
    if (!members.empty()) {
      frame += ',';
      frame += members;
    }
    frame += ",\"tick\":";
    frame += Number(tick);
    frame += ",\"type\":";
    frame += Text(type);
    frame += '}';
    return frame;
  }

  [[nodiscard]] std::string Snapshot(const ManagerStatusBoard::View &view) const {
    // Canonical order of the four names.
    std::string generations = "\"generations\":{";
    generations += "\"logical_scene\":" + Number(view.sections[1].generation);
    generations += ",\"manager\":" + Number(view.sections[0].generation);
    generations += ",\"scene_runtime\":" + Number(view.sections[2].generation);
    generations +=
        ",\"viewer_pose_sessions\":" + Number(view.sections[3].generation);
    generations += '}';
    return Envelope(view.tick, "snapshot", generations);
  }

  [[nodiscard]] std::string SectionFrame(const ManagerStatusBoard::View &view,
                                         const std::size_t index,
                                         const std::size_t capacity) const {
    const auto &section = view.sections[index];
    const auto value = [&](const bool oversized) {
      std::string text = "\"name\":";
      text += Text(kManagerStatusSectionNames[index]);
      text += ",\"section\":{\"generation\":";
      text += Number(section.generation);
      text += ",\"observed_at_ns\":";
      text += content_identity::canonical_json(
          content_identity::nonnegative_int64_value(section.observed_at_ns));
      text += oversized ? ",\"oversized\":true,\"payload\":null}"
                        : ",\"oversized\":false,\"payload\":";
      if (!oversized) {
        text += *section.payload;
        text += '}';
      }
      return Envelope(view.tick, "section", text);
    };
    auto frame = value(false);
    // Over this connection's bound: the reader learns that the section
    // changed and is unavailable, never silently stale.
    return frame.size() <= capacity ? frame : value(true);
  }

  [[nodiscard]] static bool Send(const int client, const std::string &frame,
                                 const std::size_t capacity,
                                 const std::stop_token stop) noexcept {
    return private_stream::SendFrameBytes(client, frame, capacity,
                                          kSendDeadline, stop);
  }

  const ManagerStatusBoard &board_;
  std::string application_start_id_;
};

} // namespace manyforge::headless
