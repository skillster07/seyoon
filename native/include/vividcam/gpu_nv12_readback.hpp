#pragma once

#include "vividcam/cpu_frame_transport.hpp"
#include "vividcam/gpu_context.hpp"
#include "vividcam/gpu_pixel_converter.hpp"
#include "vividcam/latency_tracker.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vividcam {

struct GpuNv12ReadbackStatistics {
  std::uint64_t successful_readbacks{0};
  std::uint64_t failed_readbacks{0};
  std::uint64_t pool_allocations{0};
  LatencySnapshot readback_latency;
};

// Packs a fixed-size NV12 surface whose planes may have padded row pitches.
// Validation completes before `packed` is changed, and an existing correctly
// sized allocation is reused.
[[nodiscard]] bool pack_nv12_rows(
    std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> y_plane, std::size_t y_row_pitch_bytes,
    std::span<const std::uint8_t> uv_plane, std::size_t uv_row_pitch_bytes,
    std::vector<std::uint8_t>& packed, std::string& error);

class GpuNv12Readback {
 public:
  virtual ~GpuNv12Readback() = default;

  // Reads one converted 1920x1080 NV12 GPU texture into a reusable packed CPU
  // frame. Source sequence/timestamp metadata is preserved in the result.
  [[nodiscard]] virtual bool read(const ConvertedGpuFrame& source,
                                  CpuNv12Frame& destination,
                                  std::string& error) = 0;
  [[nodiscard]] virtual GpuNv12ReadbackStatistics statistics() const = 0;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<GpuNv12Readback> create_gpu_nv12_readback(
    std::shared_ptr<GpuContext> gpu_context);

} // namespace vividcam
