#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace vividcam {

inline constexpr std::uint32_t kControlChannelTelemetrySchemaVersion = 1;
inline constexpr std::chrono::milliseconds kControlChannelStaleAfter{1500};
inline constexpr std::chrono::milliseconds kControlChannelReconnectAfter{3000};

enum class ControlChannelState {
  Disconnected,
  Connecting,
  Handshaking,
  Ready,
  Stale,
  Reconnecting,
  Shutdown,
};

enum class ControlChannelAdvanceResult {
  NoChange,
  BecameStale,
  BecameReconnecting,
  RetryDue,
  Rejected,
};

struct ControlChannelSnapshot {
  std::uint32_t schema_version{kControlChannelTelemetrySchemaVersion};
  ControlChannelState state{ControlChannelState::Disconnected};
  std::uint64_t connection_attempts{0};
  std::uint64_t successful_handshakes{0};
  std::uint64_t connection_failures{0};
  std::uint64_t retry_attempts{0};
  std::uint64_t heartbeats_received{0};
  std::uint64_t rejected_heartbeats{0};
  std::uint64_t stale_transitions{0};
  std::uint64_t reconnect_transitions{0};
  std::uint64_t freshness_restorations{0};
  std::uint64_t rejected_events{0};
  std::uint32_t consecutive_failures{0};
  std::optional<std::uint64_t> last_heartbeat_sequence;
  std::optional<std::int64_t> heartbeat_age_ms;
  std::optional<std::int64_t> retry_due_in_ms;
  std::int64_t retry_delay_ms{0};
  std::string first_failure_reason;
  std::string last_failure_reason;
};

[[nodiscard]] const char* control_channel_state_name(
    ControlChannelState state) noexcept;

// Platform-independent state and telemetry for the engine control connection.
// Callers provide every timestamp, so no method sleeps, creates a thread, or
// consults an operating-system clock.
class ControlChannelStateMachine {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  bool begin_connect(TimePoint now, std::string& error);
  bool mark_transport_connected(TimePoint now, std::string& error);
  bool mark_handshake_ready(TimePoint now, std::string& error);
  bool receive_heartbeat(std::uint64_t sequence, TimePoint now,
                         std::string& error);
  bool mark_connection_failed(std::string reason, TimePoint now,
                              std::string& error);
  ControlChannelAdvanceResult advance(TimePoint now, std::string& error);
  bool shutdown(TimePoint now, std::string& error);

  // A snapshot is rejected when `now` is older than any timestamp previously
  // accepted by this instance. `output` is left unchanged on rejection.
  bool snapshot(TimePoint now, ControlChannelSnapshot& output,
                std::string& error) const;
  [[nodiscard]] std::optional<TimePoint> next_retry_deadline() const;

 private:
  bool accept_time_locked(TimePoint now, std::string& error) const;
  bool reject_locked(std::string message, std::string& error);
  void enter_reconnecting_locked(std::string reason, TimePoint now);
  [[nodiscard]] std::chrono::milliseconds retry_delay_locked() const noexcept;
  [[nodiscard]] std::int64_t heartbeat_age_ms_locked(TimePoint now) const noexcept;

  mutable std::mutex mutex_;
  ControlChannelState state_{ControlChannelState::Disconnected};
  std::uint64_t connection_attempts_{0};
  std::uint64_t successful_handshakes_{0};
  std::uint64_t connection_failures_{0};
  std::uint64_t retry_attempts_{0};
  std::uint64_t heartbeats_received_{0};
  std::uint64_t rejected_heartbeats_{0};
  std::uint64_t stale_transitions_{0};
  std::uint64_t reconnect_transitions_{0};
  std::uint64_t freshness_restorations_{0};
  std::uint64_t rejected_events_{0};
  std::uint32_t consecutive_failures_{0};
  std::optional<std::uint64_t> last_heartbeat_sequence_;
  std::string first_failure_reason_;
  std::string last_failure_reason_;
  TimePoint freshness_anchor_{};
  TimePoint retry_due_at_{};
  mutable TimePoint last_observed_at_{};
  std::chrono::milliseconds retry_delay_{0};
  bool has_freshness_anchor_{false};
  bool has_retry_deadline_{false};
  mutable bool has_last_observed_at_{false};
};

} // namespace vividcam
