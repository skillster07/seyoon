#include "vividcam/virtual_camera_media_type.hpp"

#include <sstream>

namespace vividcam {
namespace {

VirtualCameraMediaType make_media_type(const OutputProfile& profile,
                                       VirtualCameraPixelFormat format) {
  const auto pixels = static_cast<std::uint64_t>(profile.width) * profile.height;
  const auto stride = format == VirtualCameraPixelFormat::Nv12
                          ? profile.width
                          : profile.width * 4U;
  const auto sample_size = format == VirtualCameraPixelFormat::Nv12
                               ? pixels * 3ULL / 2ULL
                               : pixels * 4ULL;
  return {profile.width, profile.height, profile.frames_per_second, 1,
          format, stride, sample_size};
}

bool matches(const VirtualCameraMediaType& candidate,
             const VirtualCameraMediaTypeRequest& request) {
  return (!request.width || *request.width == candidate.width) &&
         (!request.height || *request.height == candidate.height) &&
         (!request.frames_per_second ||
          *request.frames_per_second * candidate.frame_rate_denominator ==
              candidate.frame_rate_numerator) &&
         (!request.pixel_format || *request.pixel_format == candidate.pixel_format);
}
} // namespace

bool VirtualCameraMediaType::valid() const noexcept {
  if (width == 0 || height == 0 || frame_rate_numerator == 0 ||
      frame_rate_denominator == 0 || stride_bytes == 0 || sample_size_bytes == 0) {
    return false;
  }
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  if (pixel_format == VirtualCameraPixelFormat::Nv12) {
    return width % 2 == 0 && height % 2 == 0 && stride_bytes == width &&
           sample_size_bytes == pixels * 3ULL / 2ULL;
  }
  return static_cast<std::uint64_t>(stride_bytes) ==
             static_cast<std::uint64_t>(width) * 4ULL &&
         sample_size_bytes == pixels * 4ULL;
}

double VirtualCameraMediaType::frames_per_second() const noexcept {
  return frame_rate_denominator == 0
             ? 0.0
             : static_cast<double>(frame_rate_numerator) / frame_rate_denominator;
}

std::string VirtualCameraMediaType::description() const {
  std::ostringstream output;
  output << width << 'x' << height << ' ' << frames_per_second() << "p "
         << virtual_camera_pixel_format_name(pixel_format)
         << " stride=" << stride_bytes << " sample=" << sample_size_bytes;
  return output.str();
}

std::vector<VirtualCameraMediaType> supported_virtual_camera_media_types(
    const OutputProfile& profile) {
  if (!profile.valid()) return {};
  return {make_media_type(profile, VirtualCameraPixelFormat::Nv12),
          make_media_type(profile, VirtualCameraPixelFormat::Bgra)};
}

std::optional<VirtualCameraMediaType> negotiate_virtual_camera_media_type(
    const OutputProfile& profile, const VirtualCameraMediaTypeRequest& request,
    std::string& error) {
  for (const auto& candidate : supported_virtual_camera_media_types(profile)) {
    if (matches(candidate, request)) return candidate;
  }
  error = "Requested virtual camera media type is not supported by the output profile";
  return std::nullopt;
}

const char* virtual_camera_pixel_format_name(VirtualCameraPixelFormat format) noexcept {
  switch (format) {
    case VirtualCameraPixelFormat::Nv12: return "NV12";
    case VirtualCameraPixelFormat::Bgra: return "BGRA";
  }
  return "Unknown";
}

} // namespace vividcam
