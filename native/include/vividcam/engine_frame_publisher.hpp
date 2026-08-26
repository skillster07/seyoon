#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace vividcam {

inline constexpr std::uint32_t kEngineFramePublisherFramesPerSecond = 60;

struct EngineFrameTicket {
  std::uint64_t sequence{0};
  std::int64_t timestamp_100ns{0};

  friend bool operator==(const EngineFrameTicket&,
                         const EngineFrameTicket&) = default;
};

// Capture sequence numbers may restart when a camera pipeline is recreated.
// Pairing them with the worker pipeline generation keeps repeat accounting
// unambiguous across degraded recovery without changing output timestamps.
struct EngineFrameSourceIdentity {
  std::uint64_t pipeline_generation{0};
  std::uint64_t sequence{0};

  friend bool operator==(const EngineFrameSourceIdentity&,
                         const EngineFrameSourceIdentity&) = default;
};

enum class EngineFramePublishOutcome {
  Published,
  NoInput,
  ReadbackFailed,
  TransportUnavailable,
  PublishFailed,
};

struct EngineFramePublisherSnapshot {
  std::uint64_t due_frames{0};
  std::uint64_t published_frames{0};
  std::uint64_t repeated_frames{0};
  std::uint64_t deadline_drops{0};
  std::uint64_t no_input_frames{0};
  std::uint64_t readback_failures{0};
  std::uint64_t transport_unavailable_frames{0};
  std::uint64_t publish_failures{0};
};

// Pure scheduling and accounting policy for the first 60p CPU publisher.
// GPU readback and mailbox publication remain outside this class so fake time
// and fake sinks can exercise every pacing path on portable CI runners.
class EngineFramePublisher {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  // A false-to-true transition starts a fresh wall-clock schedule one frame
  // after `now`. Repeated true updates do not move the deadline. Logical frame
  // sequence and timestamps continue across disconnects and reconnects, while
  // the source-sequence repeat baseline starts fresh for the new session.
  void set_transport_ready(bool ready, TimePoint now);

  // Returns at most one ticket for the latest deadline due at `now`. Earlier
  // expired deadlines are counted as drops instead of producing a catch-up
  // burst. No work is issued while disconnected or another ticket is active.
  [[nodiscard]] std::optional<EngineFrameTicket> begin_frame(TimePoint now);

  // Completes the one active ticket. Deadlines crossed while the work was in
  // flight are dropped so the next begin_frame call cannot publish a backlog.
  // source_identity is used only to identify successful repeated input frames.
  [[nodiscard]] bool complete_frame(
      const EngineFrameTicket& ticket, TimePoint completed_at,
      EngineFramePublishOutcome outcome,
      std::optional<EngineFrameSourceIdentity> source_identity = std::nullopt);

  [[nodiscard]] std::optional<TimePoint> next_deadline() const;
  [[nodiscard]] EngineFramePublisherSnapshot snapshot() const;

 private:
  struct InFlightFrame {
    EngineFrameTicket ticket;
    TimePoint began_at{};
    std::uint64_t schedule_generation{0};
  };

  [[nodiscard]] TimePoint deadline_for_slot_locked(
      std::uint64_t slot) const noexcept;
  [[nodiscard]] std::uint64_t latest_due_slot_locked(
      TimePoint now) const noexcept;
  void record_drops_locked(std::uint64_t drops) noexcept;

  mutable std::mutex mutex_;
  bool transport_ready_{false};
  TimePoint schedule_epoch_{};
  std::uint64_t schedule_generation_{0};
  std::uint64_t next_schedule_slot_{1};
  std::uint64_t next_sequence_{1};
  std::optional<InFlightFrame> in_flight_;
  std::optional<EngineFrameSourceIdentity> last_published_source_identity_;
  EngineFramePublisherSnapshot snapshot_;
};

} // namespace vividcam
