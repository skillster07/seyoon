#include "vividcam/gpu_pixel_converter.hpp"

namespace vividcam {

bool valid_gpu_conversion_output(const VirtualCameraMediaType& media_type) noexcept {
  return media_type.valid() && media_type.pixel_format == VirtualCameraPixelFormat::Nv12;
}

} // namespace vividcam
