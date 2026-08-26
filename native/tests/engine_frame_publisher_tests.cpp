#include "vividcam/engine_frame_publisher.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>

namespace {

using namespace std::chrono_literals;
using vividcam::EngineFramePublishOutcome;
using vividcam::EngineFramePublisher;
using vividcam::EngineFrameSourceIdentity;
using vividcam::EngineFrameTicket;

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kHundredNanosecondsPerSecond = 10'000'000ULL;
constexpr std::uint64_t kFramesPerSecond =
    vividcam::kEngineFramePublisherFramesPerSecond;

EngineFramePublisher::TimePoint deadline(
    EngineFramePublisher::TimePoint epoch, std::uint64_t slot) {
  const std::uint64_t nanoseconds =
      (slot * kNanosecondsPerSecond + kFramesPerSecond - 1U) /
      kFramesPerSecond;
  return epoch + std::chrono::nanoseconds{nanoseconds};
}

std::int64_t expected_timestamp(std::uint64_t sequence) {
  return static_cast<std::int64_t>(
      (sequence - 1U) * kHundredNanosecondsPerSecond /
      kFramesPerSecond);
}

EngineFrameTicket require_ticket(
    EngineFramePublisher& publisher, EngineFramePublisher::TimePoint now) {
  const auto ticket = publisher.begin_frame(now);
  assert(ticket);
  return *ticket;
}

EngineFrameSourceIdentity source(std::uint64_t sequence,
                                 std::uint64_t pipeline_generation = 1) {
  return EngineFrameSourceIdentity{pipeline_generation, sequence};
}

void run_exact_cadence_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  assert(!publisher.next_deadline());
  assert(!publisher.begin_frame(epoch + 1h));

  publisher.set_transport_ready(true, epoch);
  assert(publisher.next_deadline() == deadline(epoch, 1));
  assert(!publisher.begin_frame(epoch));

  for (std::uint64_t sequence = 1; sequence <= 600; ++sequence) {
    const auto due_at = deadline(epoch, sequence);
    const EngineFrameTicket ticket = require_ticket(publisher, due_at);
    assert(ticket.sequence == sequence);
    assert(ticket.timestamp_100ns == expected_timestamp(sequence));
    assert(!publisher.begin_frame(due_at));
    assert(publisher.complete_frame(
        ticket, due_at, EngineFramePublishOutcome::Published,
        source(sequence)));
  }

  const auto status = publisher.snapshot();
  assert(status.due_frames == 600);
  assert(status.published_frames == 600);
  assert(status.repeated_frames == 0);
  assert(status.deadline_drops == 0);
  assert(status.no_input_frames == 0);
  assert(status.readback_failures == 0);
  assert(status.transport_unavailable_frames == 0);
  assert(status.publish_failures == 0);
  assert(publisher.next_deadline() == deadline(epoch, 601));

  const auto boundary = require_ticket(publisher, deadline(epoch, 601));
  assert(boundary.sequence == 601);
  assert(boundary.timestamp_100ns == 100'000'000);
  assert(publisher.complete_frame(boundary, deadline(epoch, 601),
                                  EngineFramePublishOutcome::Published,
                                  source(601)));
}

void run_late_begin_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  publisher.set_transport_ready(true, epoch);

  auto ticket = require_ticket(publisher, deadline(epoch, 1));
  assert(ticket.sequence == 1);
  assert(publisher.complete_frame(ticket, deadline(epoch, 1),
                                  EngineFramePublishOutcome::Published,
                                  source(10)));

  ticket = require_ticket(publisher, epoch + 70ms);
  assert(ticket.sequence == 4);
  assert(ticket.timestamp_100ns == 500'000);
  auto status = publisher.snapshot();
  assert(status.due_frames == 2);
  assert(status.deadline_drops == 2);
  assert(publisher.complete_frame(ticket, epoch + 70ms,
                                  EngineFramePublishOutcome::Published,
                                  source(11)));
  assert(publisher.next_deadline() == deadline(epoch, 5));
}

void run_slow_completion_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  publisher.set_transport_ready(true, epoch);

  const auto ticket = require_ticket(publisher, deadline(epoch, 1));
  assert(publisher.complete_frame(ticket, epoch + 70ms,
                                  EngineFramePublishOutcome::Published,
                                  source(20)));
  auto status = publisher.snapshot();
  assert(status.due_frames == 1);
  assert(status.published_frames == 1);
  assert(status.deadline_drops == 3);
  assert(publisher.next_deadline() == deadline(epoch, 5));

  const auto next = require_ticket(publisher, deadline(epoch, 5));
  assert(next.sequence == 5);
  assert(next.timestamp_100ns == expected_timestamp(5));
  assert(publisher.complete_frame(next, deadline(epoch, 5),
                                  EngineFramePublishOutcome::Published,
                                  source(21)));
}

void run_repeat_and_failure_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  publisher.set_transport_ready(true, epoch);

  const std::array outcomes = {
      EngineFramePublishOutcome::Published,
      EngineFramePublishOutcome::Published,
      EngineFramePublishOutcome::NoInput,
      EngineFramePublishOutcome::ReadbackFailed,
      EngineFramePublishOutcome::TransportUnavailable,
      EngineFramePublishOutcome::PublishFailed,
  };
  const std::array<std::optional<EngineFrameSourceIdentity>, outcomes.size()>
      sources = {source(77), source(77), std::nullopt, std::nullopt,
                 std::nullopt, std::nullopt};

  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    const auto due_at = deadline(epoch, index + 1U);
    const auto ticket = require_ticket(publisher, due_at);
    assert(publisher.complete_frame(ticket, due_at, outcomes[index],
                                    sources[index]));
  }

  const auto status = publisher.snapshot();
  assert(status.due_frames == outcomes.size());
  assert(status.published_frames == 2);
  assert(status.repeated_frames == 1);
  assert(status.deadline_drops == 0);
  assert(status.no_input_frames == 1);
  assert(status.readback_failures == 1);
  assert(status.transport_unavailable_frames == 1);
  assert(status.publish_failures == 1);
}

void run_disconnect_reconnect_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  publisher.set_transport_ready(true, epoch);

  auto ticket = require_ticket(publisher, deadline(epoch, 1));
  assert(publisher.complete_frame(ticket, deadline(epoch, 1),
                                  EngineFramePublishOutcome::Published,
                                  source(100)));
  publisher.set_transport_ready(false, epoch + 20ms);
  assert(!publisher.next_deadline());
  assert(!publisher.begin_frame(epoch + 10h));
  auto status = publisher.snapshot();
  assert(status.due_frames == 1);
  assert(status.deadline_drops == 0);

  const auto reconnected_at = epoch + 10h;
  publisher.set_transport_ready(true, reconnected_at);
  assert(!publisher.begin_frame(reconnected_at));
  assert(publisher.next_deadline() == deadline(reconnected_at, 1));
  ticket = require_ticket(publisher, deadline(reconnected_at, 1));
  assert(ticket.sequence == 2);
  assert(ticket.timestamp_100ns == expected_timestamp(2));
  assert(publisher.complete_frame(ticket, deadline(reconnected_at, 1),
                                  EngineFramePublishOutcome::Published,
                                  source(100)));
  status = publisher.snapshot();
  assert(status.due_frames == 2);
  assert(status.published_frames == 2);
  assert(status.repeated_frames == 0);
  assert(status.deadline_drops == 0);

  ticket = require_ticket(publisher, deadline(reconnected_at, 2));
  assert(ticket.sequence == 3);
  assert(publisher.complete_frame(ticket, deadline(reconnected_at, 2),
                                  EngineFramePublishOutcome::Published,
                                  source(100)));
  assert(publisher.snapshot().repeated_frames == 1);
}

void run_in_flight_reconnect_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  publisher.set_transport_ready(true, epoch);
  const auto old_ticket = require_ticket(publisher, deadline(epoch, 1));

  publisher.set_transport_ready(false, epoch + 20ms);
  const auto reconnected_at = epoch + 1s;
  publisher.set_transport_ready(true, reconnected_at);
  assert(publisher.complete_frame(
      old_ticket, reconnected_at + 100ms,
      EngineFramePublishOutcome::TransportUnavailable));
  assert(publisher.snapshot().deadline_drops == 0);

  const auto new_ticket =
      require_ticket(publisher, deadline(reconnected_at, 6));
  assert(new_ticket.sequence == 7);
  assert(new_ticket.timestamp_100ns == expected_timestamp(7));
  assert(publisher.snapshot().deadline_drops == 5);
  assert(publisher.complete_frame(
      new_ticket, deadline(reconnected_at, 6),
      EngineFramePublishOutcome::Published, source(200)));
}

void run_pipeline_generation_repeat_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  publisher.set_transport_ready(true, epoch);

  auto ticket = require_ticket(publisher, deadline(epoch, 1));
  assert(publisher.complete_frame(ticket, deadline(epoch, 1),
                                  EngineFramePublishOutcome::Published,
                                  source(77, 1)));
  ticket = require_ticket(publisher, deadline(epoch, 2));
  assert(publisher.complete_frame(ticket, deadline(epoch, 2),
                                  EngineFramePublishOutcome::Published,
                                  source(77, 1)));
  assert(publisher.snapshot().repeated_frames == 1);

  // A recreated pipeline may restart its capture sequence. The first frame
  // from that new generation is new input even when the numeric sequence is
  // identical to the previous generation.
  ticket = require_ticket(publisher, deadline(epoch, 3));
  assert(publisher.complete_frame(ticket, deadline(epoch, 3),
                                  EngineFramePublishOutcome::Published,
                                  source(77, 2)));
  assert(publisher.snapshot().repeated_frames == 1);
  ticket = require_ticket(publisher, deadline(epoch, 4));
  assert(publisher.complete_frame(ticket, deadline(epoch, 4),
                                  EngineFramePublishOutcome::Published,
                                  source(77, 2)));
  assert(publisher.snapshot().repeated_frames == 2);
}

void run_ticket_validation_test() {
  const EngineFramePublisher::TimePoint epoch{};
  EngineFramePublisher publisher;
  publisher.set_transport_ready(true, epoch);
  const auto ticket = require_ticket(publisher, deadline(epoch, 1));
  EngineFrameTicket wrong = ticket;
  ++wrong.sequence;
  assert(!publisher.complete_frame(
      wrong, deadline(epoch, 1), EngineFramePublishOutcome::Published,
      source(1)));
  assert(!publisher.complete_frame(
      ticket, epoch, EngineFramePublishOutcome::Published, source(1)));
  assert(!publisher.begin_frame(deadline(epoch, 2)));
  assert(publisher.complete_frame(
      ticket, deadline(epoch, 1), EngineFramePublishOutcome::Published,
      source(1)));
  assert(!publisher.complete_frame(
      ticket, deadline(epoch, 1), EngineFramePublishOutcome::Published,
      source(1)));
}

} // namespace

int main() {
  static_assert(kFramesPerSecond == 60);
  run_exact_cadence_test();
  run_late_begin_test();
  run_slow_completion_test();
  run_repeat_and_failure_test();
  run_disconnect_reconnect_test();
  run_in_flight_reconnect_test();
  run_pipeline_generation_repeat_test();
  run_ticket_validation_test();
  std::cout << "VIVIDCAM engine frame publisher tests passed\n";
  return 0;
}
