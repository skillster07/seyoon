#include "vividcam/engine_frame_worker.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vividcam::CpuNv12Frame;
using vividcam::EngineFramePipelineSnapshot;
using vividcam::EngineFrameWorker;
using vividcam::EngineFrameWorkerBackend;
using vividcam::EngineFrameWorkerOptions;

struct ScriptOptions {
  bool first_pipeline_degraded{false};
  std::chrono::milliseconds first_start_delay{0};
  std::chrono::milliseconds pump_delay{0};
  std::chrono::milliseconds active_stop_delay{0};
  bool always_no_frame{false};
};

struct ScriptState {
  explicit ScriptState(ScriptOptions configured_options)
      : options(configured_options) {}

  void record_thread_locked() {
    const std::thread::id current = std::this_thread::get_id();
    if (worker_thread == std::thread::id{}) {
      worker_thread = current;
    } else if (worker_thread != current) {
      thread_mismatch = true;
    }
  }

  ScriptOptions options;
  std::mutex mutex;
  std::thread::id worker_thread;
  bool thread_mismatch{false};
  std::uint64_t factory_calls{0};
  std::uint64_t start_calls{0};
  std::uint64_t stop_calls{0};
  std::uint64_t destructor_calls{0};
  std::uint64_t pump_calls{0};
  std::vector<std::chrono::steady_clock::time_point> start_times;
  std::vector<const std::uint8_t*> produced_payloads;
};

class ScriptedBackend final : public EngineFrameWorkerBackend {
 public:
  ScriptedBackend(std::shared_ptr<ScriptState> state,
                  std::uint64_t generation)
      : state_(std::move(state)), generation_(generation) {}

  ~ScriptedBackend() override {
    std::scoped_lock lock(state_->mutex);
    state_->record_thread_locked();
    ++state_->destructor_calls;
  }

  bool start(std::string& error) override {
    {
      std::scoped_lock lock(state_->mutex);
      state_->record_thread_locked();
      ++state_->start_calls;
      state_->start_times.push_back(std::chrono::steady_clock::now());
    }
    if (generation_ == 1) {
      std::this_thread::sleep_for(state_->options.first_start_delay);
    }

    snapshot_ = {};
    snapshot_.running = true;
    snapshot_.ready =
        !(generation_ == 1 && state_->options.first_pipeline_degraded);
    if (!snapshot_.ready) {
      snapshot_.gpu_error = "scripted degraded pipeline";
    }
    error.clear();
    return true;
  }

  void stop() noexcept override {
    if (snapshot_.ready) {
      std::this_thread::sleep_for(state_->options.active_stop_delay);
    }
    snapshot_.running = false;
    snapshot_.ready = false;
    std::scoped_lock lock(state_->mutex);
    state_->record_thread_locked();
    ++state_->stop_calls;
  }

  bool take_latest_cpu_frame(CpuNv12Frame& output,
                             std::string& error) override {
    std::this_thread::sleep_for(state_->options.pump_delay);
    ++snapshot_.poll_attempts;
    ++snapshot_.new_capture_frames;
    ++snapshot_.readback_frames;

    std::uint64_t sequence = 0;
    {
      std::scoped_lock lock(state_->mutex);
      state_->record_thread_locked();
      sequence = ++state_->pump_calls;
    }

    if (state_->options.always_no_frame) {
      ++snapshot_.no_frame_polls;
      error = "scripted persistent no frame";
      return false;
    }

    output.sequence = sequence;
    output.timestamp_100ns = static_cast<std::int64_t>(sequence * 1000U);
    output.width = vividcam::kCpuFrameWidth;
    output.height = vividcam::kCpuFrameHeight;
    output.y_stride_bytes = vividcam::kCpuFrameYStrideBytes;
    output.uv_stride_bytes = vividcam::kCpuFrameUvStrideBytes;
    output.bytes.resize(vividcam::kCpuFrameNv12Bytes);
    output.bytes.front() = static_cast<std::uint8_t>(sequence & 0xffU);

    {
      std::scoped_lock lock(state_->mutex);
      state_->produced_payloads.push_back(output.bytes.data());
    }
    error.clear();
    return true;
  }

  EngineFramePipelineSnapshot snapshot() const override { return snapshot_; }

 private:
  std::shared_ptr<ScriptState> state_;
  std::uint64_t generation_{0};
  EngineFramePipelineSnapshot snapshot_;
};

auto make_factory(const std::shared_ptr<ScriptState>& state) {
  return [state]() -> std::unique_ptr<EngineFrameWorkerBackend> {
    std::uint64_t generation = 0;
    {
      std::scoped_lock lock(state->mutex);
      state->record_thread_locked();
      generation = ++state->factory_calls;
    }
    return std::make_unique<ScriptedBackend>(state, generation);
  };
}

template <typename Predicate>
bool wait_until(Predicate predicate,
                std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

std::uint64_t state_counter(
    const std::shared_ptr<ScriptState>& state,
    std::uint64_t ScriptState::*member) {
  std::scoped_lock lock(state->mutex);
  return state.get()->*member;
}

void run_immediate_start_and_thread_ownership_test() {
  auto state = std::make_shared<ScriptState>(
      ScriptOptions{false, 250ms, 0ms, 0ms});
  EngineFrameWorkerOptions options;
  options.degraded_retry_delay = 20ms;
  options.pipeline_snapshot_interval = 20ms;
  EngineFrameWorker worker(make_factory(state), options);

  std::string error;
  const auto began_at = std::chrono::steady_clock::now();
  assert(worker.start(error));
  const auto start_elapsed = std::chrono::steady_clock::now() - began_at;
  assert(error.empty());
  assert(start_elapsed < 100ms);
  assert(worker.running());

  assert(!worker.start(error));
  assert(error == "Engine frame worker is already started");
  assert(wait_until([&worker] {
    return worker.snapshot().produced_frames >= 1;
  }));

  worker.stop();
  const auto status = worker.snapshot();
  assert(!status.running);
  assert(!status.pipeline_running);
  assert(!status.pipeline_ready);
  assert(status.worker_starts == 1);
  assert(status.pipeline_start_attempts == 1);

  std::scoped_lock lock(state->mutex);
  assert(!state->thread_mismatch);
  assert(state->worker_thread != std::this_thread::get_id());
  assert(state->factory_calls == 1);
  assert(state->start_calls == 1);
  assert(state->stop_calls == 1);
  assert(state->destructor_calls == 1);
}

void run_retry_pacing_swap_and_async_disable_test() {
  auto state = std::make_shared<ScriptState>(
      ScriptOptions{true, 0ms, 15ms, 150ms});
  EngineFrameWorkerOptions options;
  options.frames_per_second = 240;
  options.degraded_retry_delay = 30ms;
  options.pipeline_snapshot_interval = 20ms;
  EngineFrameWorker worker(make_factory(state), options);

  std::string error;
  assert(worker.start(error));
  assert(wait_until([&worker] {
    const auto status = worker.snapshot();
    return status.pipeline_start_attempts >= 2 &&
           status.produced_frames >= 3;
  }));

  auto status = worker.snapshot();
  assert(status.pipeline_restarts >= 1);
  assert(status.deadline_drops > 0);
  assert(status.last_error == "scripted degraded pipeline");
  assert(status.pipeline_running);
  assert(status.pipeline_ready);

  CpuNv12Frame frame;
  std::uint64_t first_pipeline_generation = 0;
  assert(worker.take_latest_cpu_frame(frame, first_pipeline_generation));
  assert(first_pipeline_generation >= 2);
  assert(frame.valid());
  {
    std::scoped_lock lock(state->mutex);
    assert(state->start_times.size() >= 2);
    assert(state->start_times[1] - state->start_times[0] >= 20ms);
    assert(std::find(state->produced_payloads.begin(),
                     state->produced_payloads.end(), frame.bytes.data()) !=
           state->produced_payloads.end());
  }

  const auto disable_began_at = std::chrono::steady_clock::now();
  worker.set_enabled(false);
  const auto disable_elapsed =
      std::chrono::steady_clock::now() - disable_began_at;
  assert(disable_elapsed < 50ms);
  assert(worker.running());
  std::uint64_t disabled_pipeline_generation = 99;
  assert(!worker.take_latest_cpu_frame(frame,
                                       disabled_pipeline_generation));
  assert(disabled_pipeline_generation == 0);
  assert(wait_until([&worker] {
    const auto current = worker.snapshot();
    return !current.pipeline_running && !current.pipeline_ready &&
           !current.retry_waiting;
  }));
  assert(wait_until([&state] {
    return state_counter(state, &ScriptState::stop_calls) >= 2;
  }));

  const std::uint64_t disabled_factory_calls =
      state_counter(state, &ScriptState::factory_calls);
  std::this_thread::sleep_for(80ms);
  assert(state_counter(state, &ScriptState::factory_calls) ==
         disabled_factory_calls);

  worker.set_enabled(true);
  assert(wait_until([&worker, disabled_factory_calls] {
    const auto current = worker.snapshot();
    return current.pipeline_start_attempts > disabled_factory_calls &&
           current.pipeline_ready && current.produced_frames >= 4 &&
           current.frame_available;
  }));
  std::uint64_t restarted_pipeline_generation = 0;
  assert(worker.take_latest_cpu_frame(frame,
                                      restarted_pipeline_generation));
  assert(restarted_pipeline_generation > first_pipeline_generation);

  worker.stop();
  status = worker.snapshot();
  assert(!status.running);
  assert(!status.enabled_requested);
  assert(!status.pipeline_running);
  assert(!status.pipeline_ready);
  assert(status.last_error == "scripted degraded pipeline");

  std::scoped_lock lock(state->mutex);
  assert(!state->thread_mismatch);
  assert(state->worker_thread != std::this_thread::get_id());
  assert(state->factory_calls >= 3);
  assert(state->start_calls == state->factory_calls);
  assert(state->stop_calls == state->factory_calls);
  assert(state->destructor_calls == state->factory_calls);
}

void run_start_disabled_test() {
  auto state = std::make_shared<ScriptState>(ScriptOptions{});
  EngineFrameWorkerOptions options;
  options.start_enabled = false;
  options.degraded_retry_delay = 20ms;
  options.pipeline_snapshot_interval = 20ms;
  EngineFrameWorker worker(make_factory(state), options);

  std::string error;
  assert(worker.start(error));
  std::this_thread::sleep_for(30ms);
  assert(state_counter(state, &ScriptState::factory_calls) == 0);
  assert(!worker.snapshot().enabled_requested);

  worker.set_enabled(true);
  assert(wait_until([&worker] {
    return worker.snapshot().produced_frames >= 1;
  }));
  worker.stop();
}

void run_persistent_no_frame_recovery_test() {
  ScriptOptions script;
  script.always_no_frame = true;
  auto state = std::make_shared<ScriptState>(script);
  EngineFrameWorkerOptions options;
  options.frames_per_second = 240;
  options.degraded_retry_delay = 20ms;
  options.persistent_no_frame_timeout = 30ms;
  options.pipeline_snapshot_interval = 1s;
  EngineFrameWorker worker(make_factory(state), options);

  std::string error;
  assert(worker.start(error));
  assert(wait_until([&worker] {
    const auto status = worker.snapshot();
    return status.pipeline_start_attempts >= 2 &&
           status.pipeline_restarts >= 1;
  }));
  const auto status = worker.snapshot();
  assert(status.no_frame_polls > 0);
  assert(status.last_error == "scripted persistent no frame");
  worker.stop();
}

} // namespace

int main() {
  static_assert(vividcam::kEngineFrameWorkerFramesPerSecond == 60);
  run_immediate_start_and_thread_ownership_test();
  run_retry_pacing_swap_and_async_disable_test();
  run_start_disabled_test();
  run_persistent_no_frame_recovery_test();
  std::cout << "VIVIDCAM engine frame worker tests passed\n";
  return 0;
}
