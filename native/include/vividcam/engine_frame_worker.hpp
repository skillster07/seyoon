#pragma once

#include "vividcam/engine_frame_pipeline.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace vividcam {

inline constexpr std::uint32_t kEngineFrameWorkerSchemaVersion = 1;
inline constexpr std::uint32_t kEngineFrameWorkerFramesPerSecond = 60;

struct EngineFrameWorkerOptions {
  std::uint32_t frames_per_second{kEngineFrameWorkerFramesPerSecond};
  std::chrono::milliseconds degraded_retry_delay{5000};
  // A pipeline can remain nominally running/ready while a driver stops
  // returning frames or a GPU stage fails every attempt. Treat a continuous
  // no-frame interval as degraded so the backend is recreated.
  std::chrono::milliseconds persistent_no_frame_timeout{2000};
  // Pipeline snapshots can calculate latency percentiles. Sampling those
  // metrics independently from the frame cadence keeps that work off the 60p
  // hot path while still exposing fresh health telemetry.
  std::chrono::milliseconds pipeline_snapshot_interval{1000};
  bool start_enabled{true};
};

struct EngineFrameWorkerSnapshot {
  std::uint32_t schema_version{kEngineFrameWorkerSchemaVersion};
  bool running{false};
  bool enabled_requested{false};
  bool pipeline_running{false};
  bool pipeline_ready{false};
  bool retry_waiting{false};
  bool frame_available{false};

  std::uint64_t worker_starts{0};
  std::uint64_t pipeline_start_attempts{0};
  std::uint64_t pipeline_restarts{0};
  std::uint64_t pump_attempts{0};
  std::uint64_t produced_frames{0};
  std::uint64_t consumed_frames{0};
  std::uint64_t overwritten_frames{0};
  std::uint64_t no_frame_polls{0};
  std::uint64_t deadline_drops{0};

  EngineFramePipelineSnapshot pipeline;
  // Sticky for the current worker lifetime so a short degraded/recovery cycle
  // remains observable after the pipeline becomes ready again.
  std::string last_error;
};

// Narrow injection seam used by portable tests. Production constructs the
// concrete EngineFramePipeline backend from inside the worker thread.
class EngineFrameWorkerBackend {
 public:
  virtual ~EngineFrameWorkerBackend() = default;
  [[nodiscard]] virtual bool start(std::string& error) = 0;
  virtual void stop() noexcept = 0;
  [[nodiscard]] virtual bool take_latest_cpu_frame(CpuNv12Frame& output,
                                                   std::string& error) = 0;
  [[nodiscard]] virtual EngineFramePipelineSnapshot snapshot() const = 0;
};

using EngineFrameWorkerBackendFactory =
    std::function<std::unique_ptr<EngineFrameWorkerBackend>()>;

// Isolates all camera/GPU pipeline calls on one owned worker thread. start()
// only creates that thread; construction, start, pumping, stop, and destruction
// of the concrete pipeline all occur on the worker.
class EngineFrameWorker {
 public:
  EngineFrameWorker();
  explicit EngineFrameWorker(EngineFrameWorkerOptions options);
  explicit EngineFrameWorker(EngineFrameWorkerBackendFactory backend_factory,
                             EngineFrameWorkerOptions options = {});
  ~EngineFrameWorker();
  EngineFrameWorker(const EngineFrameWorker&) = delete;
  EngineFrameWorker& operator=(const EngineFrameWorker&) = delete;

  // Returns after creating the worker and never performs a driver call on the
  // caller. A false result is limited to local configuration/thread failures.
  [[nodiscard]] bool start(std::string& error);

  // Queues an enable/disable command and returns without joining or calling a
  // driver. Disabling also hides any stale frame immediately; pipeline stop and
  // destruction are performed later by the worker thread.
  void set_enabled(bool enabled) noexcept;

  // Final shutdown: signals the worker and joins it. Callers should close the
  // control server before invoking this operation.
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;

  // Swaps ownership with the latest slot. The 1920x1080 NV12 payload is never
  // copied; the caller's previous reusable allocation returns to the worker.
  // pipeline_generation distinguishes capture sequence resets after recovery.
  [[nodiscard]] bool take_latest_cpu_frame(
      CpuNv12Frame& output, std::uint64_t& pipeline_generation) noexcept;
  [[nodiscard]] EngineFrameWorkerSnapshot snapshot() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vividcam
