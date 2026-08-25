#include "vividcam/control_channel_state.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace vividcam {
namespace {

constexpr std::array<std::chrono::milliseconds, 6> kRetryDelays = {
    std::chrono::milliseconds{100},
    std::chrono::milliseconds{200},
    std::chrono::milliseconds{400},
    std::chrono::milliseconds{800},
    std::chrono::milliseconds{1600},
    std::chrono::milliseconds{2000},
};

} // namespace

const char* control_channel_state_name(ControlChannelState state) noexcept {
  switch (state) {
    case ControlChannelState::Disconnected: return "disconnected";
    case ControlChannelState::Connecting: return "connecting";
    case ControlChannelState::Handshaking: return "handshaking";
    case ControlChannelState::Ready: return "ready";
    case ControlChannelState::Stale: return "stale";
    case ControlChannelState::Reconnecting: return "reconnecting";
    case ControlChannelState::Shutdown: return "shutdown";
  }
  return "unknown";
}

bool ControlChannelStateMachine::accept_time_locked(TimePoint now,
                                                    std::string& error) const {
  if (has_last_observed_at_ && now < last_observed_at_) {
    error = "control channel monotonic time moved backwards";
    return false;
  }
  last_observed_at_ = now;
  has_last_observed_at_ = true;
  error.clear();
  return true;
}

bool ControlChannelStateMachine::reject_locked(std::string message,
                                               std::string& error) {
  ++rejected_events_;
  error = std::move(message);
  return false;
}

std::chrono::milliseconds
ControlChannelStateMachine::retry_delay_locked() const noexcept {
  if (consecutive_failures_ == 0) return std::chrono::milliseconds::zero();
  const auto index = std::min<std::size_t>(
      static_cast<std::size_t>(consecutive_failures_ - 1U),
      kRetryDelays.size() - 1U);
  return kRetryDelays[index];
}

void ControlChannelStateMachine::enter_reconnecting_locked(std::string reason,
                                                           TimePoint now) {
  if (first_failure_reason_.empty()) first_failure_reason_ = reason;
  last_failure_reason_ = std::move(reason);
  ++connection_failures_;
  ++consecutive_failures_;
  ++reconnect_transitions_;
  retry_delay_ = retry_delay_locked();
  retry_due_at_ = now + retry_delay_;
  has_retry_deadline_ = true;
  state_ = ControlChannelState::Reconnecting;
}

bool ControlChannelStateMachine::begin_connect(TimePoint now,
                                               std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != ControlChannelState::Disconnected &&
      state_ != ControlChannelState::Reconnecting) {
    return reject_locked(
        "control connection can only begin while disconnected or reconnecting",
        error);
  }
  if (!accept_time_locked(now, error)) {
    ++rejected_events_;
    return false;
  }
  if (state_ == ControlChannelState::Reconnecting &&
      has_retry_deadline_ && now < retry_due_at_) {
    return reject_locked("control connection retry is not due yet", error);
  }

  if (state_ == ControlChannelState::Reconnecting) ++retry_attempts_;
  ++connection_attempts_;
  state_ = ControlChannelState::Connecting;
  has_retry_deadline_ = false;
  retry_delay_ = std::chrono::milliseconds::zero();
  has_freshness_anchor_ = false;
  last_heartbeat_sequence_.reset();
  error.clear();
  return true;
}

bool ControlChannelStateMachine::mark_transport_connected(TimePoint now,
                                                          std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != ControlChannelState::Connecting) {
    return reject_locked(
        "control transport can only connect from the connecting state", error);
  }
  if (!accept_time_locked(now, error)) {
    ++rejected_events_;
    return false;
  }
  state_ = ControlChannelState::Handshaking;
  return true;
}

bool ControlChannelStateMachine::mark_handshake_ready(TimePoint now,
                                                      std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != ControlChannelState::Handshaking) {
    return reject_locked(
        "control handshake can only complete from the handshaking state", error);
  }
  if (!accept_time_locked(now, error)) {
    ++rejected_events_;
    return false;
  }

  state_ = ControlChannelState::Ready;
  ++successful_handshakes_;
  consecutive_failures_ = 0;
  retry_delay_ = std::chrono::milliseconds::zero();
  has_retry_deadline_ = false;
  freshness_anchor_ = now;
  has_freshness_anchor_ = true;
  last_heartbeat_sequence_.reset();
  return true;
}

bool ControlChannelStateMachine::receive_heartbeat(std::uint64_t sequence,
                                                   TimePoint now,
                                                   std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != ControlChannelState::Ready &&
      state_ != ControlChannelState::Stale) {
    ++rejected_heartbeats_;
    return reject_locked(
        "control heartbeat is only valid while ready or stale", error);
  }
  if (!accept_time_locked(now, error)) {
    ++rejected_heartbeats_;
    ++rejected_events_;
    return false;
  }
  if (last_heartbeat_sequence_ && sequence <= *last_heartbeat_sequence_) {
    ++rejected_heartbeats_;
    return reject_locked(
        "control heartbeat sequence must increase monotonically", error);
  }

  if (state_ == ControlChannelState::Stale) {
    state_ = ControlChannelState::Ready;
    ++freshness_restorations_;
  }
  last_heartbeat_sequence_ = sequence;
  freshness_anchor_ = now;
  has_freshness_anchor_ = true;
  ++heartbeats_received_;
  error.clear();
  return true;
}

bool ControlChannelStateMachine::mark_connection_failed(std::string reason,
                                                        TimePoint now,
                                                        std::string& error) {
  std::scoped_lock lock(mutex_);
  if (reason.empty()) {
    return reject_locked("control connection failure reason must not be empty",
                         error);
  }
  if (state_ != ControlChannelState::Connecting &&
      state_ != ControlChannelState::Handshaking &&
      state_ != ControlChannelState::Ready &&
      state_ != ControlChannelState::Stale) {
    return reject_locked(
        "control connection failure is not valid in the current state", error);
  }
  if (!accept_time_locked(now, error)) {
    ++rejected_events_;
    return false;
  }
  enter_reconnecting_locked(std::move(reason), now);
  return true;
}

std::int64_t ControlChannelStateMachine::heartbeat_age_ms_locked(
    TimePoint now) const noexcept {
  if (!has_freshness_anchor_ || now <= freshness_anchor_) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now - freshness_anchor_)
      .count();
}

ControlChannelAdvanceResult ControlChannelStateMachine::advance(
    TimePoint now, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (!accept_time_locked(now, error)) {
    ++rejected_events_;
    return ControlChannelAdvanceResult::Rejected;
  }
  if (state_ == ControlChannelState::Shutdown) {
    return ControlChannelAdvanceResult::NoChange;
  }

  bool became_stale = false;
  if (state_ == ControlChannelState::Ready && has_freshness_anchor_ &&
      now - freshness_anchor_ >= kControlChannelStaleAfter) {
    state_ = ControlChannelState::Stale;
    ++stale_transitions_;
    became_stale = true;
  }
  if (state_ == ControlChannelState::Stale && has_freshness_anchor_ &&
      now - freshness_anchor_ >= kControlChannelReconnectAfter) {
    enter_reconnecting_locked("heartbeat timeout", now);
    return ControlChannelAdvanceResult::BecameReconnecting;
  }
  if (state_ == ControlChannelState::Reconnecting && has_retry_deadline_ &&
      now >= retry_due_at_) {
    return ControlChannelAdvanceResult::RetryDue;
  }
  return became_stale ? ControlChannelAdvanceResult::BecameStale
                      : ControlChannelAdvanceResult::NoChange;
}

bool ControlChannelStateMachine::shutdown(TimePoint now, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (!accept_time_locked(now, error)) {
    ++rejected_events_;
    return false;
  }
  state_ = ControlChannelState::Shutdown;
  has_retry_deadline_ = false;
  retry_delay_ = std::chrono::milliseconds::zero();
  return true;
}

bool ControlChannelStateMachine::snapshot(TimePoint now,
                                          ControlChannelSnapshot& output,
                                          std::string& error) const {
  std::scoped_lock lock(mutex_);
  if (!accept_time_locked(now, error)) return false;

  std::optional<std::int64_t> heartbeat_age_ms;
  if (has_freshness_anchor_) heartbeat_age_ms = heartbeat_age_ms_locked(now);

  std::optional<std::int64_t> retry_due_in_ms;
  if (state_ == ControlChannelState::Reconnecting && has_retry_deadline_) {
    retry_due_in_ms = now >= retry_due_at_
                          ? 0
                          : std::chrono::duration_cast<std::chrono::milliseconds>(
                                retry_due_at_ - now)
                                .count();
  }

  output = {kControlChannelTelemetrySchemaVersion,
            state_,
            connection_attempts_,
            successful_handshakes_,
            connection_failures_,
            retry_attempts_,
            heartbeats_received_,
            rejected_heartbeats_,
            stale_transitions_,
            reconnect_transitions_,
            freshness_restorations_,
            rejected_events_,
            consecutive_failures_,
            last_heartbeat_sequence_,
            heartbeat_age_ms,
            retry_due_in_ms,
            retry_delay_.count(),
            first_failure_reason_,
            last_failure_reason_};
  return true;
}

std::optional<ControlChannelStateMachine::TimePoint>
ControlChannelStateMachine::next_retry_deadline() const {
  std::scoped_lock lock(mutex_);
  if (state_ != ControlChannelState::Reconnecting || !has_retry_deadline_) {
    return std::nullopt;
  }
  return retry_due_at_;
}

} // namespace vividcam
