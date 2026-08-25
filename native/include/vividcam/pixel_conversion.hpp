#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vividcam {

struct Nv12Frame {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t y_stride_bytes{0};
  std::uint32_t uv_stride_bytes{0};
  std::vector<std::uint8_t> bytes;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::size_t uv_plane_offset() const noexcept;
};

[[nodiscard]] std::optional<Nv12Frame> convert_bgra_to_nv12_bt709(
    std::uint32_t width, std::uint32_t height,
    const std::vector<std::uint8_t>& bgra, std::uint32_t bgra_stride_bytes,
    std::string& error);

} // namespace vividcam
