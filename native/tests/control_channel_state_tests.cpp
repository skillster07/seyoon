#include "vividcam/control_channel_state.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

namespace {

using vividcam::ControlChannelAdvanceResult;
using vividcam::ControlChannelSnapshot;
using vividcam::ControlChannelState;
using vividcam::ControlChannelStateMachine;
using namespace std::chrono_literals;

ControlChannelSnapshot snapshot_at(ControlChannelStateMachine& channel,
                                   ControlChannelStateMachine::TimePoint now) {
  ControlChannelSnapshot result;
  std::string error;
  assert(channel.snapshot(now, result, error));
  assert(error.empty());
  return result;
}

void connect_and_complete_handshake(ControlChannelStateMachine& channel,
                                    ControlChannelStateMachine::TimePoint now) {
  std::string error;
  assert(channel.begin_connect(now, error));
  assert(channel.mark_transport_connected(now, error));
  assert(channel.mark_handshake_ready(now, error));
}

} // namespace

int main() {
  using vividcam::kControlChannelReconnectAfter;
  using vividcam::kControlChannelStaleAfter;

  assert(kControlChannelStaleAfter == 1500ms);
  assert(kControlChannelReconnectAfter == 3000ms);
  assert(std::string(vividcam::control_channel_state_name(
             ControlChannelState::Disconnected)) == "disconnected");
  assert(std::string(vividcam::control_channel_state_name(
             ControlChannelState::Reconnecting)) == "reconnecting");

  const ControlChannelStateMachine::TimePoint epoch{};
  std::string error;

  ControlChannelStateMachine boundaries;
  auto status = snapshot_at(boundaries, epoch);
  assert(status.schema_version ==
         vividcam::kControlChannelTelemetrySchemaVersion);
  assert(status.state == ControlChannelState::Disconnected);
  assert(!status.heartbeat_age_ms && !status.retry_due_in_ms);
  connect_and_complete_handshake(boundaries, epoch);
  status = snapshot_at(boundaries, epoch);
  assert(status.state == ControlChannelState::Ready);
  assert(status.connection_attempts == 1);
  assert(status.successful_handshakes == 1);
  assert(status.consecutive_failures == 0);
  assert(status.heartbeat_age_ms == 0);

  assert(boundaries.advance(epoch + 1499ms, error) ==
         ControlChannelAdvanceResult::NoChange);
  assert(snapshot_at(boundaries, epoch + 1499ms).state ==
         ControlChannelState::Ready);
  assert(boundaries.advance(epoch + 1500ms, error) ==
         ControlChannelAdvanceResult::BecameStale);
  status = snapshot_at(boundaries, epoch + 1500ms);
  assert(status.state == ControlChannelState::Stale);
  assert(status.stale_transitions == 1);
  assert(status.heartbeat_age_ms == 1500);
  assert(boundaries.advance(epoch + 2999ms, error) ==
         ControlChannelAdvanceResult::NoChange);
  assert(boundaries.advance(epoch + 3000ms, error) ==
         ControlChannelAdvanceResult::BecameReconnecting);
  status = snapshot_at(boundaries, epoch + 3000ms);
  assert(status.state == ControlChannelState::Reconnecting);
  assert(status.connection_failures == 1);
  assert(status.reconnect_transitions == 1);
  assert(status.consecutive_failures == 1);
  assert(status.retry_delay_ms == 100);
  assert(status.retry_due_in_ms == 100);
  assert(status.first_failure_reason == "heartbeat timeout");
  assert(boundaries.next_retry_deadline() == epoch + 3100ms);

  assert(!boundaries.begin_connect(epoch + 3099ms, error));
  assert(error == "control connection retry is not due yet");
  assert(boundaries.advance(epoch + 3100ms, error) ==
         ControlChannelAdvanceResult::RetryDue);
  assert(boundaries.begin_connect(epoch + 3100ms, error));
  status = snapshot_at(boundaries, epoch + 3100ms);
  assert(status.state == ControlChannelState::Connecting);
  assert(status.connection_attempts == 2 && status.retry_attempts == 1);
  assert(!status.retry_due_in_ms);

  // A delayed poll still records both crossed boundaries before reconnecting.
  ControlChannelStateMachine skipped_poll;
  connect_and_complete_handshake(skipped_poll, epoch);
  assert(skipped_poll.advance(epoch + 3000ms, error) ==
         ControlChannelAdvanceResult::BecameReconnecting);
  const auto skipped_status = snapshot_at(skipped_poll, epoch + 3000ms);
  assert(skipped_status.stale_transitions == 1);
  assert(skipped_status.reconnect_transitions == 1);

  // Consecutive failures use deterministic exponential backoff and cap at 2 s.
  const std::array expected_delays = {200ms, 400ms, 800ms, 1600ms, 2000ms,
                                      2000ms};
  auto failure_time = epoch + 3100ms;
  for (std::size_t index = 0; index < expected_delays.size(); ++index) {
    assert(boundaries.mark_connection_failed("dial failure", failure_time, error));
    status = snapshot_at(boundaries, failure_time);
    assert(status.retry_delay_ms == expected_delays[index].count());
    assert(status.retry_due_in_ms == expected_delays[index].count());
    const auto retry_time = failure_time + expected_delays[index];
    assert(boundaries.advance(retry_time, error) ==
           ControlChannelAdvanceResult::RetryDue);
    assert(boundaries.begin_connect(retry_time, error));
    failure_time = retry_time;
  }
  assert(boundaries.mark_transport_connected(failure_time, error));
  assert(boundaries.mark_handshake_ready(failure_time, error));
  status = snapshot_at(boundaries, failure_time);
  assert(status.state == ControlChannelState::Ready);
  assert(status.consecutive_failures == 0);
  assert(status.retry_delay_ms == 0 && !status.retry_due_in_ms);
  assert(status.first_failure_reason == "heartbeat timeout");
  assert(status.last_failure_reason == "dial failure");

  // A successful handshake resets the next failure to the first retry delay.
  assert(boundaries.mark_connection_failed("post-success failure", failure_time,
                                           error));
  status = snapshot_at(boundaries, failure_time);
  assert(status.consecutive_failures == 1 && status.retry_delay_ms == 100);
  assert(status.first_failure_reason == "heartbeat timeout");
  assert(status.last_failure_reason == "post-success failure");

  ControlChannelStateMachine heartbeat;
  connect_and_complete_handshake(heartbeat, epoch);
  assert(heartbeat.receive_heartbeat(10, epoch + 100ms, error));
  status = snapshot_at(heartbeat, epoch + 100ms);
  assert(status.heartbeats_received == 1);
  assert(status.last_heartbeat_sequence == 10);
  assert(status.heartbeat_age_ms == 0);
  assert(heartbeat.advance(epoch + 1600ms, error) ==
         ControlChannelAdvanceResult::BecameStale);
  assert(heartbeat.receive_heartbeat(11, epoch + 1600ms, error));
  status = snapshot_at(heartbeat, epoch + 1600ms);
  assert(status.state == ControlChannelState::Ready);
  assert(status.freshness_restorations == 1);
  assert(status.heartbeats_received == 2);
  assert(status.last_heartbeat_sequence == 11);

  assert(!heartbeat.receive_heartbeat(11, epoch + 1700ms, error));
  assert(error == "control heartbeat sequence must increase monotonically");
  assert(!heartbeat.receive_heartbeat(9, epoch + 1701ms, error));
  status = snapshot_at(heartbeat, epoch + 1701ms);
  assert(status.state == ControlChannelState::Ready);
  assert(status.heartbeats_received == 2);
  assert(status.rejected_heartbeats == 2);
  assert(status.last_heartbeat_sequence == 11);
  assert(status.heartbeat_age_ms == 101);

  // Both event mutation and snapshot reads reject regressing clock values.
  assert(!heartbeat.receive_heartbeat(12, epoch + 1699ms, error));
  assert(error == "control channel monotonic time moved backwards");
  const auto unchanged = status;
  assert(!heartbeat.snapshot(epoch + 1700ms, status, error));
  assert(status.state == unchanged.state);
  assert(status.last_heartbeat_sequence == unchanged.last_heartbeat_sequence);
  assert(heartbeat.receive_heartbeat(12, epoch + 1800ms, error));

  ControlChannelStateMachine failures;
  assert(failures.begin_connect(epoch, error));
  assert(failures.mark_connection_failed("first failure", epoch, error));
  assert(failures.begin_connect(epoch + 100ms, error));
  assert(failures.mark_transport_connected(epoch + 100ms, error));
  assert(failures.mark_connection_failed("second failure", epoch + 100ms,
                                         error));
  status = snapshot_at(failures, epoch + 100ms);
  assert(status.first_failure_reason == "first failure");
  assert(status.last_failure_reason == "second failure");
  assert(status.connection_failures == 2);
  assert(status.retry_delay_ms == 200);

  assert(failures.shutdown(epoch + 101ms, error));
  assert(failures.shutdown(epoch + 102ms, error));
  status = snapshot_at(failures, epoch + 103ms);
  assert(status.state == ControlChannelState::Shutdown);
  assert(!status.retry_due_in_ms && status.retry_delay_ms == 0);
  assert(failures.advance(epoch + 104ms, error) ==
         ControlChannelAdvanceResult::NoChange);
  assert(!failures.begin_connect(epoch + 105ms, error));
  assert(!error.empty());
  assert(!failures.receive_heartbeat(1, epoch + 106ms, error));
  assert(snapshot_at(failures, epoch + 107ms).state ==
         ControlChannelState::Shutdown);

  std::cout << "VIVIDCAM control channel state tests passed\n";
  return 0;
}
