#include "vividcam/engine_frame_worker.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vividcam {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

class PipelineBackend final : public EngineFrameWorkerBackend {
 public:
  bool start(std::string& error) override { return pipeline_.start(error); }
  void stop() noexcept override { pipeline_.stop(); }

  bool take_latest_cpu_frame(CpuNv12Frame& output,
                             std::string& error) override {
    return pipeline_.take_latest_cpu_frame(output, error);
  }

  EngineFramePipelineSnapshot snapshot() const override {
    return pipeline_.snapshot();
  }

 private:
  EngineFramePipeline pipeline_;
};

std::string describe_pipeline_error(
    const EngineFramePipelineSnapshot& snapshot) {
  const std::string* const errors[] = {
      &snapshot.pipeline_error,   &snapshot.gpu_error,
      &snapshot.camera_error,     &snapshot.capture_error,
      &snapshot.scene_error,      &snapshot.compositor_error,
      &snapshot.conversion_error, &snapshot.readback_error,
  };
  for (const std::string* error : errors) {
    if (!error->empty()) return *error;
  }
  return {};
}

std::chrono::nanoseconds schedule_offset(std::uint64_t slot,
                                         std::uint32_t frames_per_second) {
  const std::uint64_t fps = frames_per_second;
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

TimePoint schedule_deadline(TimePoint epoch, std::uint64_t slot,
                            std::uint32_t frames_per_second) {
  const auto offset = schedule_offset(slot, frames_per_second);
  const auto remaining = TimePoint::max() - epoch;
  if (offset >= remaining) return TimePoint::max();
  return epoch + offset;
}

std::uint64_t latest_due_slot(TimePoint epoch, TimePoint now,
                              std::uint32_t frames_per_second) {
  if (now <= epoch) return 0;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch).count();
  if (elapsed <= 0) return 0;

  const auto elapsed_nanoseconds = static_cast<std::uint64_t>(elapsed);
  const std::uint64_t whole_seconds =
      elapsed_nanoseconds / kNanosecondsPerSecond;
  const std::uint64_t remainder = elapsed_nanoseconds % kNanosecondsPerSecond;
  const std::uint64_t fps = frames_per_second;
  const std::uint64_t partial = remainder * fps / kNanosecondsPerSecond;
  if (whole_seconds >
      (std::numeric_limits<std::uint64_t>::max() - partial) / fps) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return whole_seconds * fps + partial;
}

} // namespace

class EngineFrameWorker::Impl {
 public:
  Impl(EngineFrameWorkerBackendFactory backend_factory,
       EngineFrameWorkerOptions options)
      : backend_factory_(std::move(backend_factory)), options_(options),
        enabled_requested_(options.start_enabled) {
    snapshot_.enabled_requested = enabled_requested_;
  }

  ~Impl() { stop(); }

  bool start(std::string& error) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (!validate_options(error)) return false;

    {
      std::scoped_lock state_lock(state_mutex_);
      if (worker_.joinable()) {
        error = "Engine frame worker is already started";
        return false;
      }
      stop_requested_ = false;
      enabled_requested_ = options_.start_enabled;
      has_latest_frame_ = false;
      latest_frame_pipeline_generation_ = 0;
      snapshot_.running = true;
      snapshot_.enabled_requested = enabled_requested_;
      snapshot_.pipeline_running = false;
      snapshot_.pipeline_ready = false;
      snapshot_.retry_waiting = false;
      snapshot_.frame_available = false;
      snapshot_.pipeline = {};
      snapshot_.last_error.clear();
      ++snapshot_.worker_starts;
    }

    try {
      worker_ = std::thread([this] { run(); });
    } catch (const std::exception& exception) {
      std::scoped_lock state_lock(state_mutex_);
      snapshot_.running = false;
      snapshot_.enabled_requested = false;
      enabled_requested_ = false;
      if (snapshot_.worker_starts != 0) --snapshot_.worker_starts;
      snapshot_.last_error = exception.what();
      error = "Could not create engine frame worker: " +
              std::string{exception.what()};
      return false;
    } catch (...) {
      std::scoped_lock state_lock(state_mutex_);
      snapshot_.running = false;
      snapshot_.enabled_requested = false;
      enabled_requested_ = false;
      if (snapshot_.worker_starts != 0) --snapshot_.worker_starts;
      snapshot_.last_error = "Unknown worker thread creation failure";
      error = "Could not create engine frame worker";
      return false;
    }

    error.clear();
    return true;
  }

  void set_enabled(bool enabled) noexcept {
    {
      std::scoped_lock lock(state_mutex_);
      // Once final shutdown is requested, a racing reconnect command must not
      // revive the worker while stop() is joining it.
      if (stop_requested_ && enabled) return;
      enabled_requested_ = enabled;
      snapshot_.enabled_requested = enabled;
      if (!enabled) {
        has_latest_frame_ = false;
        latest_frame_pipeline_generation_ = 0;
        snapshot_.frame_available = false;
      }
    }
    state_changed_.notify_all();
  }

  void stop() noexcept {
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    std::thread worker;
    {
      std::scoped_lock state_lock(state_mutex_);
      stop_requested_ = true;
      enabled_requested_ = false;
      has_latest_frame_ = false;
      latest_frame_pipeline_generation_ = 0;
      snapshot_.enabled_requested = false;
      snapshot_.frame_available = false;
    }
    state_changed_.notify_all();
    if (worker_.joinable()) worker = std::move(worker_);

    if (worker.joinable()) worker.join();

    std::scoped_lock state_lock(state_mutex_);
    snapshot_.running = false;
    snapshot_.pipeline_running = false;
    snapshot_.pipeline_ready = false;
    snapshot_.retry_waiting = false;
  }

  bool running() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return snapshot_.running;
  }

  bool take_latest_cpu_frame(CpuNv12Frame& output,
                             std::uint64_t& pipeline_generation) noexcept {
    std::scoped_lock lock(state_mutex_);
    if (!enabled_requested_ || !has_latest_frame_) {
      pipeline_generation = 0;
      return false;
    }
    using std::swap;
    swap(output, latest_frame_);
    pipeline_generation = latest_frame_pipeline_generation_;
    has_latest_frame_ = false;
    latest_frame_pipeline_generation_ = 0;
    snapshot_.frame_available = false;
    ++snapshot_.consumed_frames;
    return true;
  }

  EngineFrameWorkerSnapshot snapshot() const {
    std::scoped_lock lock(state_mutex_);
    return snapshot_;
  }

 private:
  enum class ActiveResult { Stop, Disabled, Degraded };

  bool validate_options(std::string& error) const {
    if (!backend_factory_) {
      error = "Engine frame worker backend factory is empty";
      return false;
    }
    if (options_.frames_per_second == 0 ||
        options_.frames_per_second > 1000) {
      error = "Engine frame worker rate must be between 1 and 1000 fps";
      return false;
    }
    if (options_.degraded_retry_delay <= std::chrono::milliseconds::zero()) {
      error = "Engine frame worker retry delay must be positive";
      return false;
    }
    if (options_.persistent_no_frame_timeout <=
        std::chrono::milliseconds::zero()) {
      error = "Engine frame worker no-frame timeout must be positive";
      return false;
    }
    if (options_.pipeline_snapshot_interval <=
        std::chrono::milliseconds::zero()) {
      error = "Engine frame worker snapshot interval must be positive";
      return false;
    }
    return true;
  }

  void run() noexcept {
    try {
      run_loop();
    } catch (const std::exception& exception) {
      record_error(exception.what());
    } catch (...) {
      record_error("Unknown engine frame worker failure");
    }

    std::scoped_lock lock(state_mutex_);
    snapshot_.running = false;
    snapshot_.pipeline_running = false;
    snapshot_.pipeline_ready = false;
    snapshot_.retry_waiting = false;
    snapshot_.frame_available = false;
    has_latest_frame_ = false;
    latest_frame_pipeline_generation_ = 0;
    state_changed_.notify_all();
  }

  void run_loop() {
    while (wait_until_enabled()) {
      const std::uint64_t pipeline_generation = begin_pipeline_attempt();

      std::unique_ptr<EngineFrameWorkerBackend> backend;
      std::string attempt_error;
      EngineFramePipelineSnapshot pipeline_snapshot;
      bool started = false;
      try {
        backend = backend_factory_();
        if (!backend) {
          attempt_error = "Engine frame worker backend factory returned null";
        } else {
          started = backend->start(attempt_error);
          pipeline_snapshot = backend->snapshot();
          update_pipeline_snapshot(pipeline_snapshot);
          if (attempt_error.empty()) {
            attempt_error = describe_pipeline_error(pipeline_snapshot);
          }
        }
      } catch (const std::exception& exception) {
        attempt_error = exception.what();
      } catch (...) {
        attempt_error = "Unknown engine frame pipeline start failure";
      }

      if (!started || !pipeline_snapshot.running ||
          !pipeline_snapshot.ready || !command_is_enabled()) {
        if (!attempt_error.empty()) record_error(attempt_error);
        stop_backend_on_worker(backend, pipeline_snapshot);
        if (should_stop()) return;
        if (!command_is_enabled()) continue;
        if (!wait_for_retry()) return;
        continue;
      }

      const ActiveResult result = pump_pipeline(*backend, pipeline_generation);
      stop_backend_on_worker(backend, pipeline_snapshot);
      if (result == ActiveResult::Stop) return;
      if (result == ActiveResult::Disabled) continue;
      if (!wait_for_retry()) return;
    }
  }

  bool wait_until_enabled() {
    std::unique_lock lock(state_mutex_);
    state_changed_.wait(lock,
                        [this] { return stop_requested_ || enabled_requested_; });
    snapshot_.retry_waiting = false;
    return !stop_requested_;
  }

  bool wait_for_retry() {
    std::unique_lock lock(state_mutex_);
    if (stop_requested_) return false;
    if (!enabled_requested_) return true;
    snapshot_.retry_waiting = true;
    const auto retry_at = Clock::now() + options_.degraded_retry_delay;
    state_changed_.wait_until(lock, retry_at, [this] {
      return stop_requested_ || !enabled_requested_;
    });
    snapshot_.retry_waiting = false;
    return !stop_requested_;
  }

  std::uint64_t begin_pipeline_attempt() {
    std::scoped_lock lock(state_mutex_);
    ++snapshot_.pipeline_start_attempts;
    if (snapshot_.pipeline_start_attempts > 1) {
      ++snapshot_.pipeline_restarts;
    }
    snapshot_.pipeline_running = false;
    snapshot_.pipeline_ready = false;
    snapshot_.retry_waiting = false;
    return snapshot_.pipeline_start_attempts;
  }

  ActiveResult pump_pipeline(EngineFrameWorkerBackend& backend,
                             std::uint64_t pipeline_generation) {
    const TimePoint epoch = Clock::now();
    std::uint64_t next_slot = 1;
    TimePoint next_snapshot_at =
        epoch + options_.pipeline_snapshot_interval;
    TimePoint no_frame_since{};
    bool has_no_frame_since = false;
    CpuNv12Frame working_frame;

    for (;;) {
      const TimePoint deadline = schedule_deadline(
          epoch, next_slot, options_.frames_per_second);
      {
        std::unique_lock lock(state_mutex_);
        state_changed_.wait_until(lock, deadline, [this] {
          return stop_requested_ || !enabled_requested_;
        });
        if (stop_requested_) return ActiveResult::Stop;
        if (!enabled_requested_) return ActiveResult::Disabled;
      }

      const TimePoint began_at = Clock::now();
      const std::uint64_t due = latest_due_slot(
          epoch, began_at, options_.frames_per_second);
      if (due < next_slot) continue;
      record_deadline_drops(due - next_slot);
      next_slot = due + 1U;

      {
        std::scoped_lock lock(state_mutex_);
        ++snapshot_.pump_attempts;
      }

      std::string frame_error;
      bool produced = false;
      bool pump_threw = false;
      try {
        produced = backend.take_latest_cpu_frame(working_frame, frame_error);
      } catch (const std::exception& exception) {
        frame_error = exception.what();
        pump_threw = true;
      } catch (...) {
        frame_error = "Unknown engine frame pipeline pump failure";
        pump_threw = true;
      }

      if (produced && !working_frame.valid()) {
        produced = false;
        frame_error =
            "Engine frame pipeline returned an invalid 1920x1080 NV12 frame";
      }

      const TimePoint completed_at = Clock::now();
      if (produced) {
        has_no_frame_since = false;
      } else if (!has_no_frame_since) {
        no_frame_since = completed_at;
        has_no_frame_since = true;
      }

      {
        std::scoped_lock lock(state_mutex_);
        if (stop_requested_) return ActiveResult::Stop;
        if (!enabled_requested_) return ActiveResult::Disabled;
        if (produced) {
          if (has_latest_frame_) ++snapshot_.overwritten_frames;
          using std::swap;
          swap(working_frame, latest_frame_);
          latest_frame_pipeline_generation_ = pipeline_generation;
          has_latest_frame_ = true;
          snapshot_.frame_available = true;
          ++snapshot_.produced_frames;
        } else {
          ++snapshot_.no_frame_polls;
          if (!frame_error.empty()) snapshot_.last_error = frame_error;
        }
      }

      if (pump_threw) return ActiveResult::Degraded;

      if (has_no_frame_since &&
          completed_at - no_frame_since >=
              options_.persistent_no_frame_timeout) {
        EngineFramePipelineSnapshot stalled_snapshot;
        try {
          stalled_snapshot = backend.snapshot();
        } catch (const std::exception& exception) {
          record_error(exception.what());
          return ActiveResult::Degraded;
        } catch (...) {
          record_error("Unknown engine frame pipeline snapshot failure");
          return ActiveResult::Degraded;
        }
        update_pipeline_snapshot(stalled_snapshot);
        if (frame_error.empty()) {
          frame_error = describe_pipeline_error(stalled_snapshot);
        }
        if (frame_error.empty()) {
          frame_error =
              "Engine frame pipeline produced no frame before the timeout";
        }
        record_error(frame_error);
        return ActiveResult::Degraded;
      }

      if (completed_at >= next_snapshot_at) {
        EngineFramePipelineSnapshot live_snapshot;
        try {
          live_snapshot = backend.snapshot();
        } catch (const std::exception& exception) {
          record_error(exception.what());
          return ActiveResult::Degraded;
        } catch (...) {
          record_error("Unknown engine frame pipeline snapshot failure");
          return ActiveResult::Degraded;
        }
        update_pipeline_snapshot(live_snapshot);
        if (!live_snapshot.running || !live_snapshot.ready) {
          std::string degraded_error = describe_pipeline_error(live_snapshot);
          if (degraded_error.empty()) {
            degraded_error = "Engine frame pipeline became degraded";
          }
          record_error(degraded_error);
          return ActiveResult::Degraded;
        }
        next_snapshot_at =
            completed_at + options_.pipeline_snapshot_interval;
      }

      const std::uint64_t due_after_completion = latest_due_slot(
          epoch, completed_at, options_.frames_per_second);
      if (due_after_completion >= next_slot) {
        record_deadline_drops(due_after_completion - next_slot + 1U);
        next_slot = due_after_completion + 1U;
      }
    }
  }

  void stop_backend_on_worker(
      std::unique_ptr<EngineFrameWorkerBackend>& backend,
      EngineFramePipelineSnapshot fallback_snapshot) {
    if (backend) {
      try {
        fallback_snapshot = backend->snapshot();
      } catch (...) {
      }
      backend->stop();
      backend.reset();
    }
    fallback_snapshot.running = false;
    fallback_snapshot.ready = false;
    update_pipeline_snapshot(std::move(fallback_snapshot));
  }

  void update_pipeline_snapshot(EngineFramePipelineSnapshot snapshot) {
    std::scoped_lock lock(state_mutex_);
    snapshot_.pipeline_running = snapshot.running;
    snapshot_.pipeline_ready = snapshot.ready;
    snapshot_.pipeline = std::move(snapshot);
  }

  void record_deadline_drops(std::uint64_t drops) noexcept {
    std::scoped_lock lock(state_mutex_);
    snapshot_.deadline_drops += drops;
  }

  void record_error(const std::string& error) {
    if (error.empty()) return;
    std::scoped_lock lock(state_mutex_);
    snapshot_.last_error = error;
  }

  bool should_stop() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return stop_requested_;
  }

  bool command_is_enabled() const noexcept {
    std::scoped_lock lock(state_mutex_);
    return enabled_requested_ && !stop_requested_;
  }

  EngineFrameWorkerBackendFactory backend_factory_;
  EngineFrameWorkerOptions options_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex state_mutex_;
  std::condition_variable state_changed_;
  std::thread worker_;
  bool stop_requested_{false};
  bool enabled_requested_{true};
  bool has_latest_frame_{false};
  std::uint64_t latest_frame_pipeline_generation_{0};
  CpuNv12Frame latest_frame_;
  EngineFrameWorkerSnapshot snapshot_;
};

EngineFrameWorker::EngineFrameWorker()
    : EngineFrameWorker(EngineFrameWorkerOptions{}) {}

EngineFrameWorker::EngineFrameWorker(EngineFrameWorkerOptions options)
    : EngineFrameWorker(
          [] { return std::make_unique<PipelineBackend>(); }, options) {}

EngineFrameWorker::EngineFrameWorker(
    EngineFrameWorkerBackendFactory backend_factory,
    EngineFrameWorkerOptions options)
    : impl_(std::make_unique<Impl>(std::move(backend_factory), options)) {}

EngineFrameWorker::~EngineFrameWorker() = default;

bool EngineFrameWorker::start(std::string& error) {
  return impl_->start(error);
}

void EngineFrameWorker::set_enabled(bool enabled) noexcept {
  impl_->set_enabled(enabled);
}

void EngineFrameWorker::stop() noexcept { impl_->stop(); }

bool EngineFrameWorker::running() const noexcept { return impl_->running(); }

bool EngineFrameWorker::take_latest_cpu_frame(
    CpuNv12Frame& output, std::uint64_t& pipeline_generation) noexcept {
  return impl_->take_latest_cpu_frame(output, pipeline_generation);
}

EngineFrameWorkerSnapshot EngineFrameWorker::snapshot() const {
  return impl_->snapshot();
}

} // namespace vividcam
