#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace vividcam {

inline constexpr std::uint32_t kEngineTelemetrySchemaVersion = 1;

enum class EngineLifecycleState {
  Created,
  Starting,
  Running,
  Stopping,
  Stopped,
  Failed,
};

enum class EngineStopReason {
  None,
  Requested,
  RunForElapsed,
  ConsoleSignal,
  Failure,
};

enum class EngineTickResult {
  NoHeartbeat,
  Heartbeat,
  Rejected,
};

struct EngineHostSnapshot {
  std::uint32_t schema_version{kEngineTelemetrySchemaVersion};
  std::string instance_id;
  EngineLifecycleState state{EngineLifecycleState::Created};
  EngineStopReason stop_reason{EngineStopReason::None};
  std::uint64_t heartbeat_sequence{0};
  std::uint64_t missed_heartbeat_intervals{0};
  std::int64_t uptime_ms{0};
  std::int64_t last_heartbeat_uptime_ms{0};
  bool frame_transport_ready{false};
  std::string failure;
};

[[nodiscard]] const char* engine_lifecycle_state_name(
    EngineLifecycleState state) noexcept;
[[nodiscard]] const char* engine_stop_reason_name(EngineStopReason reason) noexcept;
[[nodiscard]] bool valid_engine_instance_id(const std::string& instance_id) noexcept;

class EngineHost {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  explicit EngineHost(
      std::string instance_id,
      std::chrono::milliseconds heartbeat_interval = std::chrono::seconds{1});

  bool begin_start(TimePoint now, std::string& error);
  bool mark_running(TimePoint now, std::string& error);
  EngineTickResult tick(TimePoint now, std::string& error);
  bool request_stop(EngineStopReason reason, TimePoint now, std::string& error);
  bool mark_stopped(TimePoint now, std::string& error);
  bool mark_failed(std::string failure, TimePoint now, std::string& error);

  [[nodiscard]] EngineHostSnapshot snapshot(TimePoint now) const;
  [[nodiscard]] std::optional<TimePoint> next_heartbeat_deadline() const;

 private:
  bool accept_time_locked(TimePoint now, std::string& error);
  [[nodiscard]] std::int64_t uptime_ms_locked(TimePoint now) const noexcept;

  const std::string instance_id_;
  const std::chrono::milliseconds heartbeat_interval_;
  mutable std::mutex mutex_;
  EngineLifecycleState state_{EngineLifecycleState::Created};
  EngineStopReason stop_reason_{EngineStopReason::None};
  std::uint64_t heartbeat_sequence_{0};
  std::uint64_t missed_heartbeat_intervals_{0};
  std::int64_t last_heartbeat_uptime_ms_{0};
  std::string failure_;
  TimePoint started_at_{};
  mutable TimePoint last_observed_at_{};
  TimePoint next_heartbeat_at_{};
  TimePoint terminal_at_{};
  bool has_started_at_{false};
  mutable bool has_last_observed_at_{false};
  bool has_terminal_at_{false};
};

} // namespace vividcam
