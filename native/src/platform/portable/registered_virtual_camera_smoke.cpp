#include "vividcam/registered_virtual_camera_smoke.hpp"

namespace vividcam {

RegisteredVirtualCameraSmokeResult run_registered_virtual_camera_smoke(
    std::uint32_t, std::uint32_t) {
  RegisteredVirtualCameraSmokeResult result;
  result.error = "Registered virtual camera smoke testing is available on Windows only";
  return result;
}

} // namespace vividcam
