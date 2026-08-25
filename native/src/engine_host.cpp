#include "vividcam/engine_host.hpp"

#include <stdexcept>
#include <utility>

namespace vividcam {

const char* engine_lifecycle_state_name(EngineLifecycleState state) noexcept {
  switch (state) {
    case EngineLifecycleState::Created: return "created";
    case EngineLifecycleState::Starting: return "starting";
    case EngineLifecycleState::Running: return "running";
    case EngineLifecycleState::Stopping: return "stopping";
    case EngineLifecycleState::Stopped: return "stopped";
    case EngineLifecycleState::Failed: return "failed";
  }
  return "unknown";
}

const char* engine_stop_reason_name(EngineStopReason reason) noexcept {
  switch (reason) {
    case EngineStopReason::None: return "none";
    case EngineStopReason::Requested: return "requested";
    case EngineStopReason::RunForElapsed: return "run-for";
    case EngineStopReason::ConsoleSignal: return "signal";
    case EngineStopReason::Failure: return "failure";
  }
  return "unknown";
}

bool valid_engine_instance_id(const std::string& instance_id) noexcept {
  if (instance_id.empty() || instance_id.size() > 64) return false;
  for (const char value : instance_id) {
    const bool lower = value >= 'a' && value <= 'z';
    const bool upper = value >= 'A' && value <= 'Z';
    const bool digit = value >= '0' && value <= '9';
    if (!lower && !upper && !digit && value != '-' && value != '_' && value != '.') {
      return false;
    }
  }
  return true;
}

EngineHost::EngineHost(std::string instance_id,
                       std::chrono::milliseconds heartbeat_interval)
    : instance_id_(std::move(instance_id)),
      heartbeat_interval_(heartbeat_interval) {
  if (!valid_engine_instance_id(instance_id_)) {
    throw std::invalid_argument(
        "engine instance id must contain 1-64 ASCII letters, digits, '.', '_' or '-'");
  }
  if (heartbeat_interval_ <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("engine heartbeat interval must be positive");
  }
}

bool EngineHost::accept_time_locked(TimePoint now, std::string& error) {
  if (has_last_observed_at_ && now < last_observed_at_) {
    error = "engine monotonic time moved backwards";
    return false;
  }
  last_observed_at_ = now;
  has_last_observed_at_ = true;
  error.clear();
  return true;
}

bool EngineHost::begin_start(TimePoint now, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != EngineLifecycleState::Created) {
    error = "engine can only begin starting from the created state";
    return false;
  }
  if (!accept_time_locked(now, error)) return false;
  started_at_ = now;
  has_started_at_ = true;
  state_ = EngineLifecycleState::Starting;
  return true;
}

bool EngineHost::mark_running(TimePoint now, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != EngineLifecycleState::Starting) {
    error = "engine can only enter running from the starting state";
    return false;
  }
  if (!accept_time_locked(now, error)) return false;
  next_heartbeat_at_ = now + heartbeat_interval_;
  state_ = EngineLifecycleState::Running;
  return true;
}

EngineTickResult EngineHost::tick(TimePoint now, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ != EngineLifecycleState::Running) {
    error = "engine heartbeat is only valid in the running state";
    return EngineTickResult::Rejected;
  }
  if (!accept_time_locked(now, error)) return EngineTickResult::Rejected;
  if (now < next_heartbeat_at_) return EngineTickResult::NoHeartbeat;

  const auto periods_late = (now - next_heartbeat_at_) / heartbeat_interval_;
  const auto intervals_due = static_cast<std::uint64_t>(periods_late) + 1U;
  ++heartbeat_sequence_;
  missed_heartbeat_intervals_ += intervals_due - 1U;
  last_heartbeat_uptime_ms_ = uptime_ms_locked(now);
  next_heartbeat_at_ +=
      heartbeat_interval_ * static_cast<std::int64_t>(intervals_due);
  return EngineTickResult::Heartbeat;
}

bool EngineHost::request_stop(EngineStopReason reason, TimePoint now,
                              std::string& error) {
  std::scoped_lock lock(mutex_);
  if (reason == EngineStopReason::None || reason == EngineStopReason::Failure) {
    error = "engine stop request requires a non-failure reason";
    return false;
  }
  if (state_ == EngineLifecycleState::Stopped) {
    error.clear();
    return true;
  }
  if (state_ == EngineLifecycleState::Stopping) {
    if (!accept_time_locked(now, error)) return false;
    return true;
  }
  if (state_ != EngineLifecycleState::Starting &&
      state_ != EngineLifecycleState::Running) {
    error = "engine stop can only be requested while starting or running";
    return false;
  }
  if (!accept_time_locked(now, error)) return false;
  stop_reason_ = reason;
  state_ = EngineLifecycleState::Stopping;
  return true;
}

bool EngineHost::mark_stopped(TimePoint now, std::string& error) {
  std::scoped_lock lock(mutex_);
  if (state_ == EngineLifecycleState::Stopped) {
    error.clear();
    return true;
  }
  if (state_ != EngineLifecycleState::Stopping) {
    error = "engine can only stop from the stopping state";
    return false;
  }
  if (!accept_time_locked(now, error)) return false;
  terminal_at_ = now;
  has_terminal_at_ = true;
  state_ = EngineLifecycleState::Stopped;
  return true;
}

bool EngineHost::mark_failed(std::string failure, TimePoint now,
                             std::string& error) {
  std::scoped_lock lock(mutex_);
  if (failure.empty()) {
    error = "engine failure message must not be empty";
    return false;
  }
  if (state_ == EngineLifecycleState::Failed) {
    error.clear();
    return true;
  }
  if (state_ == EngineLifecycleState::Stopped) {
    error = "a stopped engine cannot enter the failed state";
    return false;
  }
  if (!accept_time_locked(now, error)) return false;
  if (!has_started_at_) {
    started_at_ = now;
    has_started_at_ = true;
  }
  failure_ = std::move(failure);
  stop_reason_ = EngineStopReason::Failure;
  terminal_at_ = now;
  has_terminal_at_ = true;
  state_ = EngineLifecycleState::Failed;
  return true;
}

std::int64_t EngineHost::uptime_ms_locked(TimePoint now) const noexcept {
  if (!has_started_at_) return 0;
  const TimePoint effective_now = has_terminal_at_ ? terminal_at_ : now;
  if (effective_now <= started_at_) return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             effective_now - started_at_)
      .count();
}

EngineHostSnapshot EngineHost::snapshot(TimePoint now) const {
  std::scoped_lock lock(mutex_);
  if (has_last_observed_at_ && now < last_observed_at_) {
    now = last_observed_at_;
  } else {
    last_observed_at_ = now;
    has_last_observed_at_ = true;
  }
  return {kEngineTelemetrySchemaVersion,
          instance_id_,
          state_,
          stop_reason_,
          heartbeat_sequence_,
          missed_heartbeat_intervals_,
          uptime_ms_locked(now),
          last_heartbeat_uptime_ms_,
          false,
          failure_};
}

std::optional<EngineHost::TimePoint> EngineHost::next_heartbeat_deadline() const {
  std::scoped_lock lock(mutex_);
  if (state_ != EngineLifecycleState::Running) return std::nullopt;
  return next_heartbeat_at_;
}

} // namespace vividcam
