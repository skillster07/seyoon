#pragma once

#include "vividcam/output_profile.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vividcam {

enum class VirtualCameraPixelFormat { Nv12, Bgra };

struct VirtualCameraMediaType {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t frame_rate_numerator{0};
  std::uint32_t frame_rate_denominator{1};
  VirtualCameraPixelFormat pixel_format{VirtualCameraPixelFormat::Nv12};
  std::uint32_t stride_bytes{0};
  std::uint64_t sample_size_bytes{0};

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] double frames_per_second() const noexcept;
  [[nodiscard]] std::string description() const;
};

struct VirtualCameraMediaTypeRequest {
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> height;
  std::optional<std::uint32_t> frames_per_second;
  std::optional<VirtualCameraPixelFormat> pixel_format;
};

[[nodiscard]] std::vector<VirtualCameraMediaType> supported_virtual_camera_media_types(
    const OutputProfile& profile);
[[nodiscard]] std::optional<VirtualCameraMediaType> negotiate_virtual_camera_media_type(
    const OutputProfile& profile, const VirtualCameraMediaTypeRequest& request,
    std::string& error);
[[nodiscard]] const char* virtual_camera_pixel_format_name(
    VirtualCameraPixelFormat format) noexcept;

} // namespace vividcam
