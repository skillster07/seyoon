#include "vividcam/virtual_camera_registration.hpp"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>

#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>

namespace vividcam {
namespace {

MFVirtualCameraLifetime native_lifetime(VirtualCameraLifetime lifetime) {
  return lifetime == VirtualCameraLifetime::Session
             ? MFVirtualCameraLifetime_Session
             : MFVirtualCameraLifetime_System;
}

MFVirtualCameraAccess native_access(VirtualCameraAccess access) {
  return access == VirtualCameraAccess::CurrentUser
             ? MFVirtualCameraAccess_CurrentUser
             : MFVirtualCameraAccess_AllUsers;
}

std::string hresult_error(const char* operation, HRESULT status) {
  std::ostringstream message;
  message << operation << " failed (HRESULT=0x" << std::hex << std::uppercase
          << std::setw(8) << std::setfill('0')
          << static_cast<std::uint32_t>(status) << ')';
  return message.str();
}
} // namespace

NativeMediaFoundationHandle register_and_start_virtual_camera(
    const VirtualCameraRegistrationConfig& config, std::string& error) {
  if (!config.valid()) {
    error = "Virtual camera registration config is invalid";
    return {};
  }
  CLSID source_id{};
  if (FAILED(CLSIDFromString(config.source_clsid.c_str(), &source_id))) {
    error = "Virtual camera source CLSID is malformed";
    return {};
  }

  IMFVirtualCamera* camera = nullptr;
  HRESULT status = MFCreateVirtualCamera(
      MFVirtualCameraType_SoftwareCameraSource,
      native_lifetime(config.lifetime), native_access(config.access),
      config.friendly_name.c_str(), config.source_clsid.c_str(), nullptr, 0, &camera);
  if (FAILED(status)) {
    error = hresult_error("MFCreateVirtualCamera", status);
    return {};
  }
  status = camera->Start(nullptr);
  if (FAILED(status)) {
    camera->Release();
    error = hresult_error("IMFVirtualCamera::Start", status);
    return {};
  }
  return {std::shared_ptr<void>(camera, [](void* value) {
            auto* virtual_camera = static_cast<IMFVirtualCamera*>(value);
            virtual_camera->Stop();
            virtual_camera->Release();
          }),
          reinterpret_cast<std::uintptr_t>(camera)};
}

bool stop_registered_virtual_camera(const NativeMediaFoundationHandle& camera_handle,
                                    std::string& error) {
  if (!camera_handle.valid()) {
    error = "Registered virtual camera handle is invalid";
    return false;
  }
  const auto status =
      reinterpret_cast<IMFVirtualCamera*>(camera_handle.native_pointer)->Stop();
  if (FAILED(status)) {
    error = hresult_error("IMFVirtualCamera::Stop", status);
    return false;
  }
  return true;
}

bool remove_registered_virtual_camera(const NativeMediaFoundationHandle& camera_handle,
                                      std::string& error) {
  if (!camera_handle.valid()) {
    error = "Registered virtual camera handle is invalid";
    return false;
  }
  const auto status =
      reinterpret_cast<IMFVirtualCamera*>(camera_handle.native_pointer)->Remove();
  if (FAILED(status)) {
    error = hresult_error("IMFVirtualCamera::Remove", status);
    return false;
  }
  return true;
}

} // namespace vividcam
