#pragma once

#include "vividcam/cpu_frame_transport.hpp"
#include "vividcam/latency_tracker.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace vividcam {

inline constexpr std::uint32_t kEngineFramePipelineSchemaVersion = 1;

struct EngineFramePipelineSnapshot {
  std::uint32_t schema_version{kEngineFramePipelineSchemaVersion};
  bool running{false};
  // Ready means that camera capture and every GPU stage required to produce a
  // fixed 1920x1080 NV12 CPU frame are usable. A running pipeline may remain
  // degraded indefinitely while hardware is absent.
  bool ready{false};

  std::uint64_t poll_attempts{0};
  std::uint64_t new_capture_frames{0};
  std::uint64_t repeated_capture_frames{0};
  std::uint64_t cpu_only_frames{0};
  std::uint64_t rendered_frames{0};
  std::uint64_t render_failures{0};
  std::uint64_t converted_frames{0};
  std::uint64_t conversion_failures{0};
  std::uint64_t readback_frames{0};
  std::uint64_t readback_failures{0};
  std::uint64_t no_frame_polls{0};
  LatencySnapshot readback_latency;

  std::string pipeline_error;
  std::string gpu_error;
  std::string camera_error;
  std::string capture_error;
  std::string scene_error;
  std::string compositor_error;
  std::string conversion_error;
  std::string readback_error;
};

// Owns the Windows-only capture -> compositor -> NV12 conversion -> CPU
// readback path. It intentionally does not own a control server or publishing
// clock; callers decide when and where a returned frame is published.
class EngineFramePipeline {
 public:
  EngineFramePipeline();
  ~EngineFramePipeline();
  EngineFramePipeline(const EngineFramePipeline&) = delete;
  EngineFramePipeline& operator=(const EngineFramePipeline&) = delete;

  // Missing hardware is a degraded but successfully started state. False is
  // reserved for an internal lifecycle failure that prevents the pipeline from
  // remaining alive.
  [[nodiscard]] bool start(std::string& error);
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept;

  // Performs one bounded pipeline attempt. If capture has no newer frame, the
  // most recent GPU-backed input is rendered again. Its capture source sequence
  // is preserved in the returned CpuNv12Frame so the publisher can identify a
  // repeated input. Hardware absence and CPU-only capture return no frame and a
  // recoverable error.
  // The caller owns and may reuse output across ticks. False means that no
  // frame was produced and error describes the recoverable stage condition.
  [[nodiscard]] bool take_latest_cpu_frame(CpuNv12Frame& output,
                                           std::string& error);
  [[nodiscard]] EngineFramePipelineSnapshot snapshot() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vividcam
