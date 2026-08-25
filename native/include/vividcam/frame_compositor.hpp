#pragma once

#include "vividcam/camera_capture.hpp"
#include "vividcam/gpu_context.hpp"
#include "vividcam/latency_tracker.hpp"
#include "vividcam/scene_graph.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace vividcam {

enum class CanvasOrientation { Landscape, Portrait };

struct CompositorConfig {
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t frames_per_second{60};
  CanvasOrientation orientation{CanvasOrientation::Landscape};
};

struct CompositedFrame {
  std::uint64_t source_sequence{0};
  std::int64_t source_timestamp_100ns{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::shared_ptr<void> texture_owner;
  std::uintptr_t native_texture{0};
};

struct CompositorStatistics {
  std::uint64_t rendered_frames{0};
  std::uint64_t rejected_frames{0};
  std::uint64_t pool_allocations{0};
  std::uint64_t skipped_layers{0};
  LatencySnapshot render_latency;
};

class FrameCompositor {
 public:
  virtual ~FrameCompositor() = default;
  virtual bool configure(const CompositorConfig& config, std::string& error) = 0;
  virtual bool set_render_plan(const std::vector<RenderCommand>& commands,
                               std::string& error) = 0;
  [[nodiscard]] virtual std::optional<CompositedFrame> render(
      const CapturedFrame& source, std::string& error) = 0;
  [[nodiscard]] virtual CompositorStatistics statistics() const = 0;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<FrameCompositor> create_frame_compositor(
    std::shared_ptr<GpuContext> gpu_context);
[[nodiscard]] bool valid_compositor_config(const CompositorConfig& config) noexcept;
[[nodiscard]] bool valid_render_plan(const std::vector<RenderCommand>& commands) noexcept;

} // namespace vividcam
