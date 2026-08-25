#include "vividcam/pixel_conversion.hpp"

#include <algorithm>
#include <cmath>

namespace vividcam {
namespace {
constexpr std::uint64_t kMaximumConversionPixels = 3840ULL * 2160ULL;

std::uint8_t limited_y(double red, double green, double blue) {
  return static_cast<std::uint8_t>(std::clamp(
      std::lround(16.0 + 219.0 * (0.2126 * red + 0.7152 * green + 0.0722 * blue) / 255.0),
      16L, 235L));
}

std::uint8_t limited_u(double red, double green, double blue) {
  return static_cast<std::uint8_t>(std::clamp(
      std::lround(128.0 + 224.0 * (-0.114572 * red - 0.385428 * green + 0.5 * blue) / 255.0),
      16L, 240L));
}

std::uint8_t limited_v(double red, double green, double blue) {
  return static_cast<std::uint8_t>(std::clamp(
      std::lround(128.0 + 224.0 * (0.5 * red - 0.454153 * green - 0.045847 * blue) / 255.0),
      16L, 240L));
}
} // namespace

bool Nv12Frame::valid() const noexcept {
  if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0 ||
      y_stride_bytes < width || uv_stride_bytes < width) {
    return false;
  }
  const auto required = static_cast<std::uint64_t>(y_stride_bytes) * height +
                        static_cast<std::uint64_t>(uv_stride_bytes) * (height / 2U);
  return required == bytes.size();
}

std::size_t Nv12Frame::uv_plane_offset() const noexcept {
  return static_cast<std::size_t>(y_stride_bytes) * height;
}

std::optional<Nv12Frame> convert_bgra_to_nv12_bt709(
    std::uint32_t width, std::uint32_t height,
    const std::vector<std::uint8_t>& bgra, std::uint32_t bgra_stride_bytes,
    std::string& error) {
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  const auto minimum_stride = static_cast<std::uint64_t>(width) * 4ULL;
  const auto required_bgra = static_cast<std::uint64_t>(bgra_stride_bytes) * height;
  if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0 ||
      pixels > kMaximumConversionPixels || minimum_stride > bgra_stride_bytes ||
      required_bgra > bgra.size()) {
    error = "BGRA input dimensions, stride, or byte count is invalid for NV12 conversion";
    return std::nullopt;
  }

  Nv12Frame output{width, height, width, width,
                   std::vector<std::uint8_t>(pixels * 3ULL / 2ULL, 0)};
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto source = static_cast<std::size_t>(y) * bgra_stride_bytes + x * 4ULL;
      output.bytes[static_cast<std::size_t>(y) * output.y_stride_bytes + x] =
          limited_y(bgra[source + 2], bgra[source + 1], bgra[source]);
    }
  }

  const auto uv_offset = output.uv_plane_offset();
  for (std::uint32_t y = 0; y < height; y += 2) {
    for (std::uint32_t x = 0; x < width; x += 2) {
      double red = 0.0;
      double green = 0.0;
      double blue = 0.0;
      for (std::uint32_t offset_y = 0; offset_y < 2; ++offset_y) {
        for (std::uint32_t offset_x = 0; offset_x < 2; ++offset_x) {
          const auto source = static_cast<std::size_t>(y + offset_y) * bgra_stride_bytes +
                              (x + offset_x) * 4ULL;
          blue += bgra[source];
          green += bgra[source + 1];
          red += bgra[source + 2];
        }
      }
      const auto destination = uv_offset + static_cast<std::size_t>(y / 2U) *
                                               output.uv_stride_bytes + x;
      output.bytes[destination] = limited_u(red / 4.0, green / 4.0, blue / 4.0);
      output.bytes[destination + 1] = limited_v(red / 4.0, green / 4.0, blue / 4.0);
    }
  }
  return output;
}

} // namespace vividcam
