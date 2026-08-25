#pragma once

#include "vividcam/media_foundation_adapter.hpp"

#include <string>

namespace vividcam {

enum class VirtualCameraLifetime { Session, System };
enum class VirtualCameraAccess { CurrentUser, AllUsers };

struct VirtualCameraRegistrationConfig {
  std::wstring friendly_name{L"VIVIDCAM Virtual Camera"};
  std::wstring source_clsid;
  VirtualCameraLifetime lifetime{VirtualCameraLifetime::Session};
  VirtualCameraAccess access{VirtualCameraAccess::CurrentUser};

  [[nodiscard]] bool valid() const noexcept;
};

// Creates a registration object without making the camera enumerable. Call
// start_registered_virtual_camera() to register (or re-enable) the camera.
// Session handles stop automatically when their final owner is released.
// System handles never stop implicitly, so a started system camera remains
// registered after the handle is released.
[[nodiscard]] NativeMediaFoundationHandle create_virtual_camera_registration(
    const VirtualCameraRegistrationConfig& config, std::string& error);
[[nodiscard]] bool start_registered_virtual_camera(
    const NativeMediaFoundationHandle& camera, std::string& error);

// Compatibility convenience for callers that want create + start in one call.
[[nodiscard]] NativeMediaFoundationHandle register_and_start_virtual_camera(
    const VirtualCameraRegistrationConfig& config, std::string& error);

// Installs a persistent camera for the calling user. The returned handle may be
// released without disabling the camera; stop/remove are always explicit.
[[nodiscard]] NativeMediaFoundationHandle
register_and_start_persistent_virtual_camera(
    const std::wstring& friendly_name, const std::wstring& source_clsid,
    std::string& error);
[[nodiscard]] bool stop_registered_virtual_camera(
    const NativeMediaFoundationHandle& camera, std::string& error);
[[nodiscard]] bool remove_registered_virtual_camera(
    const NativeMediaFoundationHandle& camera, std::string& error);
[[nodiscard]] std::wstring registered_virtual_camera_symbolic_link(
    const NativeMediaFoundationHandle& camera, std::string& error);

} // namespace vividcam
