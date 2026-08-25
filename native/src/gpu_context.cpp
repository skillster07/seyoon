#include "vividcam/gpu_context.hpp"

namespace vividcam {

const char* gpu_backend_name(GpuBackend backend) noexcept {
  switch (backend) {
    case GpuBackend::Unsupported: return "Unsupported";
    case GpuBackend::D3D11Hardware: return "D3D11 Hardware";
    case GpuBackend::D3D11Warp: return "D3D11 WARP";
  }
  return "Unknown";
}

} // namespace vividcam
