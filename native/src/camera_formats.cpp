#include "vividcam/camera_devices.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace vividcam {
namespace {

template <typename Predicate>
std::optional<CameraFormat> select_preferred_format_if(
    const std::vector<CameraFormat>& formats, std::uint32_t target_width,
    std::uint32_t target_height, std::uint32_t target_fps,
    Predicate&& predicate) noexcept {
  const auto format_cost = [=](const CameraFormat& format) {
    if (!format.valid() || !predicate(format)) {
      return std::numeric_limits<double>::infinity();
    }
    const auto size_delta =
        std::abs(static_cast<double>(format.width) - target_width) +
        std::abs(static_cast<double>(format.height) - target_height);
    const auto fps_delta =
        std::abs(format.frames_per_second() - target_fps);
    const double pixel_cost =
        format.pixel_format == PixelFormat::Nv12    ? 0.0
        : format.pixel_format == PixelFormat::Yuy2  ? 25.0
        : format.pixel_format == PixelFormat::Mjpeg ? 50.0
        : format.pixel_format == PixelFormat::Bgra  ? 75.0
        : format.pixel_format == PixelFormat::H264  ? 100.0
                                                     : 500.0;
    const double below_target_penalty =
        format.frames_per_second() + 0.01 < target_fps ? 10000.0 : 0.0;
    return size_delta * 10.0 + fps_delta * 100.0 + pixel_cost +
           below_target_penalty;
  };

  const auto selected = std::min_element(
      formats.begin(), formats.end(),
      [&](const CameraFormat& left, const CameraFormat& right) {
        return format_cost(left) < format_cost(right);
      });
  if (selected == formats.end() || !selected->valid() ||
      !predicate(*selected)) {
    return std::nullopt;
  }
  return *selected;
}

} // namespace

double CameraFormat::frames_per_second() const noexcept {
  return frames_per_second_denominator == 0
             ? 0.0
             : static_cast<double>(frames_per_second_numerator) /
                   static_cast<double>(frames_per_second_denominator);
}

bool CameraFormat::valid() const noexcept {
  return width > 0 && height > 0 && frames_per_second_numerator > 0 &&
         frames_per_second_denominator > 0;
}

std::string CameraFormat::description() const {
  std::ostringstream stream;
  stream << width << 'x' << height << " @ " << frames_per_second() << " FPS "
         << pixel_format_name(pixel_format);
  return stream.str();
}

const char* pixel_format_name(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::Nv12: return "NV12";
    case PixelFormat::Yuy2: return "YUY2";
    case PixelFormat::Bgra: return "BGRA";
    case PixelFormat::Mjpeg: return "MJPEG";
    case PixelFormat::H264: return "H264";
    case PixelFormat::Unknown: return "Unknown";
  }
  return "Unknown";
}

std::optional<CameraFormat> select_preferred_format(
    const std::vector<CameraFormat>& formats, std::uint32_t target_width,
    std::uint32_t target_height, std::uint32_t target_fps) noexcept {
  return select_preferred_format_if(
      formats, target_width, target_height, target_fps,
      [](const CameraFormat&) noexcept { return true; });
}

bool is_gpu_compositor_capture_format(PixelFormat format) noexcept {
  return format == PixelFormat::Nv12 || format == PixelFormat::Yuy2 ||
         format == PixelFormat::Bgra;
}

std::optional<CameraFormat> select_preferred_gpu_compositor_format(
    const std::vector<CameraFormat>& formats, std::uint32_t target_width,
    std::uint32_t target_height, std::uint32_t target_fps) noexcept {
  return select_preferred_format_if(
      formats, target_width, target_height, target_fps,
      [](const CameraFormat& format) noexcept {
        return is_gpu_compositor_capture_format(format.pixel_format);
      });
}

} // namespace vividcam
