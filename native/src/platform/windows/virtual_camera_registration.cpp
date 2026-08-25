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

NativeMediaFoundationHandle make_registration_handle(
    IMFVirtualCamera* camera, VirtualCameraLifetime lifetime) {
  return {std::shared_ptr<void>(camera, [lifetime](void* value) {
            auto* virtual_camera = static_cast<IMFVirtualCamera*>(value);
            if (lifetime == VirtualCameraLifetime::Session) {
              // A session camera belongs to this handle's process lifetime.
              // Stop is intentionally best-effort in a noexcept deleter.
              (void)virtual_camera->Stop();
            }
            // Shutdown releases the control object's internal resources. For a
            // System camera it does not disable or unregister the device.
            (void)virtual_camera->Shutdown();
            virtual_camera->Release();
          }),
          reinterpret_cast<std::uintptr_t>(camera)};
}

IMFVirtualCamera* native_camera(
    const NativeMediaFoundationHandle& camera_handle, std::string& error) {
  if (!camera_handle.valid()) {
    error = "Registered virtual camera handle is invalid";
    return nullptr;
  }
  return reinterpret_cast<IMFVirtualCamera*>(camera_handle.native_pointer);
}
} // namespace

NativeMediaFoundationHandle create_virtual_camera_registration(
    const VirtualCameraRegistrationConfig& config, std::string& error) {
  error.clear();
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
  return make_registration_handle(camera, config.lifetime);
}

bool start_registered_virtual_camera(
    const NativeMediaFoundationHandle& camera_handle, std::string& error) {
  error.clear();
  auto* camera = native_camera(camera_handle, error);
  if (!camera) return false;
  const auto status = camera->Start(nullptr);
  if (FAILED(status)) {
    error = hresult_error("IMFVirtualCamera::Start", status);
    return false;
  }
  return true;
}

NativeMediaFoundationHandle register_and_start_virtual_camera(
    const VirtualCameraRegistrationConfig& config, std::string& error) {
  auto camera = create_virtual_camera_registration(config, error);
  if (!camera.valid() || !start_registered_virtual_camera(camera, error)) return {};
  return camera;
}

NativeMediaFoundationHandle register_and_start_persistent_virtual_camera(
    const std::wstring& friendly_name, const std::wstring& source_clsid,
    std::string& error) {
  const VirtualCameraRegistrationConfig config{
      friendly_name, source_clsid, VirtualCameraLifetime::System,
      VirtualCameraAccess::CurrentUser};
  return register_and_start_virtual_camera(config, error);
}

bool stop_registered_virtual_camera(const NativeMediaFoundationHandle& camera_handle,
                                    std::string& error) {
  error.clear();
  auto* camera = native_camera(camera_handle, error);
  if (!camera) return false;
  const auto status = camera->Stop();
  if (FAILED(status)) {
    error = hresult_error("IMFVirtualCamera::Stop", status);
    return false;
  }
  return true;
}

bool remove_registered_virtual_camera(const NativeMediaFoundationHandle& camera_handle,
                                      std::string& error) {
  error.clear();
  auto* camera = native_camera(camera_handle, error);
  if (!camera) return false;
  const auto status = camera->Remove();
  if (FAILED(status)) {
    error = hresult_error("IMFVirtualCamera::Remove", status);
    return false;
  }
  return true;
}

std::wstring registered_virtual_camera_symbolic_link(
    const NativeMediaFoundationHandle& camera_handle, std::string& error) {
  error.clear();
  auto* camera = native_camera(camera_handle, error);
  if (!camera) return {};

  wchar_t* value = nullptr;
  UINT32 length = 0;
  const auto status = camera->GetAllocatedString(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &value, &length);
  if (FAILED(status)) {
    error = hresult_error(
        "IMFVirtualCamera::GetAllocatedString(symbolic link)", status);
    return {};
  }
  const std::wstring symbolic_link(value, length);
  CoTaskMemFree(value);
  return symbolic_link;
}

} // namespace vividcam
