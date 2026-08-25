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

[[nodiscard]] NativeMediaFoundationHandle register_and_start_virtual_camera(
    const VirtualCameraRegistrationConfig& config, std::string& error);
[[nodiscard]] bool stop_registered_virtual_camera(
    const NativeMediaFoundationHandle& camera, std::string& error);
[[nodiscard]] bool remove_registered_virtual_camera(
    const NativeMediaFoundationHandle& camera, std::string& error);

} // namespace vividcam
