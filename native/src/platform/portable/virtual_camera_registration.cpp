#include "vividcam/virtual_camera_registration.hpp"

namespace vividcam {

namespace {
constexpr const char* kUnavailable =
    "Windows virtual camera registration is unavailable on this platform";
}

NativeMediaFoundationHandle create_virtual_camera_registration(
    const VirtualCameraRegistrationConfig&, std::string& error) {
  error = kUnavailable;
  return {};
}

bool start_registered_virtual_camera(const NativeMediaFoundationHandle&,
                                     std::string& error) {
  error = kUnavailable;
  return false;
}

NativeMediaFoundationHandle register_and_start_virtual_camera(
    const VirtualCameraRegistrationConfig& config, std::string& error) {
  return create_virtual_camera_registration(config, error);
}

NativeMediaFoundationHandle register_and_start_persistent_virtual_camera(
    const std::wstring&, const std::wstring&, std::string& error) {
  error = kUnavailable;
  return {};
}

bool stop_registered_virtual_camera(const NativeMediaFoundationHandle&,
                                    std::string& error) {
  error = kUnavailable;
  return false;
}

bool remove_registered_virtual_camera(const NativeMediaFoundationHandle&,
                                      std::string& error) {
  error = kUnavailable;
  return false;
}

std::wstring registered_virtual_camera_symbolic_link(
    const NativeMediaFoundationHandle&, std::string& error) {
  error = kUnavailable;
  return {};
}

} // namespace vividcam
