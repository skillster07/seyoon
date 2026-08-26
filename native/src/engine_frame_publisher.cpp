#include "vividcam/engine_frame_publisher.hpp"

#include <limits>

namespace vividcam {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kHundredNanosecondsPerSecond = 10'000'000ULL;

std::chrono::nanoseconds schedule_offset(std::uint64_t slot) noexcept {
  constexpr std::uint64_t fps = kEngineFramePublisherFramesPerSecond;
  const std::uint64_t whole_seconds = slot / fps;
  const std::uint64_t remainder = slot % fps;
  const std::uint64_t partial_nanoseconds =
      (remainder * kNanosecondsPerSecond + fps - 1U) / fps;
  const auto maximum = static_cast<std::uint64_t>(
      std::numeric_limits<std::chrono::nanoseconds::rep>::max());
  if (whole_seconds >
      (maximum - partial_nanoseconds) / kNanosecondsPerSecond) {
    return std::chrono::nanoseconds::max();
  }
  return std::chrono::nanoseconds{
      static_cast<std::chrono::nanoseconds::rep>(
          whole_seconds * kNanosecondsPerSecond + partial_nanoseconds)};
}

std::int64_t timestamp_for_sequence(std::uint64_t sequence) noexcept {
  if (sequence == 0) return 0;
  constexpr std::uint64_t fps = kEngineFramePublisherFramesPerSecond;
  const std::uint64_t index = sequence - 1U;
  const std::uint64_t whole_seconds = index / fps;
  const std::uint64_t remainder = index % fps;
  const auto maximum = static_cast<std::uint64_t>(
      std::numeric_limits<std::int64_t>::max());
  const std::uint64_t partial =
      remainder * kHundredNanosecondsPerSecond / fps;
  if (whole_seconds >
      (maximum - partial) / kHundredNanosecondsPerSecond) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(
      whole_seconds * kHundredNanosecondsPerSecond + partial);
}

} // namespace

void EngineFramePublisher::set_transport_ready(bool ready, TimePoint now) {
  std::scoped_lock lock(mutex_);
  if (ready == transport_ready_) return;

  transport_ready_ = ready;
  if (!transport_ready_) return;

  schedule_epoch_ = now;
  next_schedule_slot_ = 1;
  ++schedule_generation_;
  last_published_source_identity_.reset();
}

std::optional<EngineFrameTicket> EngineFramePublisher::begin_frame(
    TimePoint now) {
  std::scoped_lock lock(mutex_);
  if (!transport_ready_ || in_flight_) return std::nullopt;

  const std::uint64_t latest_due = latest_due_slot_locked(now);
  if (latest_due < next_schedule_slot_) return std::nullopt;

  const std::uint64_t skipped = latest_due - next_schedule_slot_;
  record_drops_locked(skipped);
  next_schedule_slot_ = latest_due + 1U;

  const EngineFrameTicket ticket{next_sequence_,
                                 timestamp_for_sequence(next_sequence_)};
  ++next_sequence_;
  ++snapshot_.due_frames;
  in_flight_ = InFlightFrame{ticket, now, schedule_generation_};
  return ticket;
}

bool EngineFramePublisher::complete_frame(
    const EngineFrameTicket& ticket, TimePoint completed_at,
    EngineFramePublishOutcome outcome,
    std::optional<EngineFrameSourceIdentity> source_identity) {
  std::scoped_lock lock(mutex_);
  if (!in_flight_ || in_flight_->ticket != ticket ||
      completed_at < in_flight_->began_at) {
    return false;
  }

  const std::uint64_t ticket_schedule_generation =
      in_flight_->schedule_generation;
  in_flight_.reset();

  switch (outcome) {
    case EngineFramePublishOutcome::Published:
      ++snapshot_.published_frames;
      if (source_identity) {
        if (last_published_source_identity_ == source_identity) {
          ++snapshot_.repeated_frames;
        }
        last_published_source_identity_ = source_identity;
      }
      break;
    case EngineFramePublishOutcome::NoInput:
      ++snapshot_.no_input_frames;
      break;
    case EngineFramePublishOutcome::ReadbackFailed:
      ++snapshot_.readback_failures;
      break;
    case EngineFramePublishOutcome::TransportUnavailable:
      ++snapshot_.transport_unavailable_frames;
      break;
    case EngineFramePublishOutcome::PublishFailed:
      ++snapshot_.publish_failures;
      break;
  }

  // A reconnect creates a new epoch while an old ticket may still be winding
  // down. Never let that old work consume deadlines from the new schedule.
  if (!transport_ready_ ||
      ticket_schedule_generation != schedule_generation_) {
    return true;
  }

  const std::uint64_t latest_due = latest_due_slot_locked(completed_at);
  if (latest_due >= next_schedule_slot_) {
    const std::uint64_t skipped = latest_due - next_schedule_slot_ + 1U;
    record_drops_locked(skipped);
    next_schedule_slot_ = latest_due + 1U;
  }
  return true;
}

std::optional<EngineFramePublisher::TimePoint>
EngineFramePublisher::next_deadline() const {
  std::scoped_lock lock(mutex_);
  if (!transport_ready_) return std::nullopt;
  return deadline_for_slot_locked(next_schedule_slot_);
}

EngineFramePublisherSnapshot EngineFramePublisher::snapshot() const {
  std::scoped_lock lock(mutex_);
  return snapshot_;
}

EngineFramePublisher::TimePoint EngineFramePublisher::deadline_for_slot_locked(
    std::uint64_t slot) const noexcept {
  const auto offset = schedule_offset(slot);
  const auto remaining = TimePoint::max() - schedule_epoch_;
  if (offset >= remaining) return TimePoint::max();
  return schedule_epoch_ + offset;
}

std::uint64_t EngineFramePublisher::latest_due_slot_locked(
    TimePoint now) const noexcept {
  if (now <= schedule_epoch_) return 0;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - schedule_epoch_)
          .count();
  if (elapsed <= 0) return 0;

  const auto elapsed_nanoseconds = static_cast<std::uint64_t>(elapsed);
  const std::uint64_t whole_seconds =
      elapsed_nanoseconds / kNanosecondsPerSecond;
  const std::uint64_t remainder =
      elapsed_nanoseconds % kNanosecondsPerSecond;
  constexpr std::uint64_t fps = kEngineFramePublisherFramesPerSecond;
  if (whole_seconds >
      (std::numeric_limits<std::uint64_t>::max() -
       remainder * fps / kNanosecondsPerSecond) /
          fps) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return whole_seconds * fps +
         remainder * fps / kNanosecondsPerSecond;
}

void EngineFramePublisher::record_drops_locked(std::uint64_t drops) noexcept {
  snapshot_.deadline_drops += drops;
  next_sequence_ += drops;
}

} // namespace vividcam
