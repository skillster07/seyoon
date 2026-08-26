#include "vividcam/gpu_nv12_readback.hpp"

#include <cstring>
#include <limits>
#include <new>

namespace vividcam {
namespace {

bool plane_has_rows(std::span<const std::uint8_t> plane,
                    std::size_t row_pitch_bytes, std::size_t row_bytes,
                    std::size_t row_count) noexcept {
  if (row_pitch_bytes < row_bytes) return false;
  if (row_count != 0 &&
      row_pitch_bytes > std::numeric_limits<std::size_t>::max() / row_count) {
    return false;
  }
  return plane.size() >= row_pitch_bytes * row_count;
}

} // namespace

bool pack_nv12_rows(std::uint32_t width, std::uint32_t height,
                    std::span<const std::uint8_t> y_plane,
                    std::size_t y_row_pitch_bytes,
                    std::span<const std::uint8_t> uv_plane,
                    std::size_t uv_row_pitch_bytes,
                    std::vector<std::uint8_t>& packed,
                    std::string& error) {
  if (width != kCpuFrameWidth || height != kCpuFrameHeight ||
      (width & 1U) != 0 || (height & 1U) != 0) {
    error = "GPU NV12 readback requires fixed 1920x1080 dimensions";
    return false;
  }

  const auto row_bytes = static_cast<std::size_t>(width);
  const auto y_rows = static_cast<std::size_t>(height);
  const auto uv_rows = y_rows / 2U;
  if (!plane_has_rows(y_plane, y_row_pitch_bytes, row_bytes, y_rows)) {
    error = "GPU NV12 readback Y plane pitch or size is invalid";
    return false;
  }
  if (!plane_has_rows(uv_plane, uv_row_pitch_bytes, row_bytes, uv_rows)) {
    error = "GPU NV12 readback UV plane pitch or size is invalid";
    return false;
  }

  try {
    packed.resize(kCpuFrameNv12Bytes);
  } catch (const std::bad_alloc&) {
    error = "Unable to allocate packed GPU NV12 readback storage";
    return false;
  }

  for (std::size_t row = 0; row < y_rows; ++row) {
    std::memcpy(packed.data() + row * row_bytes,
                y_plane.data() + row * y_row_pitch_bytes, row_bytes);
  }
  const std::size_t uv_destination_offset = row_bytes * y_rows;
  for (std::size_t row = 0; row < uv_rows; ++row) {
    std::memcpy(packed.data() + uv_destination_offset + row * row_bytes,
                uv_plane.data() + row * uv_row_pitch_bytes, row_bytes);
  }
  error.clear();
  return true;
}

} // namespace vividcam
