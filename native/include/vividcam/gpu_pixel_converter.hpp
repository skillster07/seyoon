#pragma once

#include "vividcam/frame_compositor.hpp"
#include "vividcam/gpu_context.hpp"
#include "vividcam/latency_tracker.hpp"
#include "vividcam/virtual_camera_media_type.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace vividcam {

struct ConvertedGpuFrame {
  CompositedFrame frame;
  VirtualCameraPixelFormat pixel_format{VirtualCameraPixelFormat::Nv12};
};

struct GpuPixelConverterStatistics {
  std::uint64_t converted_frames{0};
  std::uint64_t rejected_frames{0};
  std::uint64_t pool_allocations{0};
  LatencySnapshot conversion_latency;
};

class GpuPixelConverter {
 public:
  virtual ~GpuPixelConverter() = default;
  virtual bool configure(const VirtualCameraMediaType& output_type,
                         std::string& error) = 0;
  [[nodiscard]] virtual std::optional<ConvertedGpuFrame> convert(
      const CompositedFrame& source, std::string& error) = 0;
  [[nodiscard]] virtual GpuPixelConverterStatistics statistics() const = 0;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<GpuPixelConverter> create_gpu_pixel_converter(
    std::shared_ptr<GpuContext> gpu_context);
[[nodiscard]] bool valid_gpu_conversion_output(
    const VirtualCameraMediaType& media_type) noexcept;

} // namespace vividcam
