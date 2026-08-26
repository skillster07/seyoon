#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vividcam {

enum class PixelFormat { Nv12, Yuy2, Bgra, Mjpeg, H264, Unknown };

struct CameraFormat {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t frames_per_second_numerator{0};
  std::uint32_t frames_per_second_denominator{1};
  PixelFormat pixel_format{PixelFormat::Unknown};

  [[nodiscard]] double frames_per_second() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::string description() const;
};

struct CameraDevice {
  std::wstring friendly_name;
  std::wstring symbolic_link;
  std::vector<CameraFormat> formats;
};

struct CameraEnumerationResult {
  std::vector<CameraDevice> devices;
  std::string error;
  [[nodiscard]] bool supported() const noexcept { return error.empty(); }
};

[[nodiscard]] CameraEnumerationResult enumerate_camera_devices();
[[nodiscard]] std::optional<CameraFormat> select_preferred_format(
    const std::vector<CameraFormat>& formats, std::uint32_t target_width = 1920,
    std::uint32_t target_height = 1080, std::uint32_t target_fps = 60) noexcept;
[[nodiscard]] bool is_gpu_compositor_capture_format(
    PixelFormat format) noexcept;
[[nodiscard]] std::optional<CameraFormat>
select_preferred_gpu_compositor_format(
    const std::vector<CameraFormat>& formats, std::uint32_t target_width = 1920,
    std::uint32_t target_height = 1080,
    std::uint32_t target_fps = 60) noexcept;
[[nodiscard]] const char* pixel_format_name(PixelFormat format) noexcept;

} // namespace vividcam
