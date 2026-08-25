#include "vividcam/virtual_camera_registration.hpp"

namespace vividcam {

NativeMediaFoundationHandle register_and_start_virtual_camera(
    const VirtualCameraRegistrationConfig&, std::string& error) {
  error = "Windows virtual camera registration is unavailable on this platform";
  return {};
}

bool stop_registered_virtual_camera(const NativeMediaFoundationHandle&,
                                    std::string& error) {
  error = "Windows virtual camera registration is unavailable on this platform";
  return false;
}

bool remove_registered_virtual_camera(const NativeMediaFoundationHandle&,
                                      std::string& error) {
  error = "Windows virtual camera registration is unavailable on this platform";
  return false;
}

} // namespace vividcam
