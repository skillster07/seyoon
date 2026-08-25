#include "vividcam/gpu_context.hpp"

namespace vividcam {

GpuContextResult create_gpu_context(bool) {
  return {nullptr, "D3D11 GPU context is available on Windows only"};
}

} // namespace vividcam
