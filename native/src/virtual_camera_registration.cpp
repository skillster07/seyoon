#include "vividcam/virtual_camera_registration.hpp"

namespace vividcam {

bool VirtualCameraRegistrationConfig::valid() const noexcept {
  return !friendly_name.empty() && !source_clsid.empty() &&
         source_clsid.front() == L'{' && source_clsid.back() == L'}' &&
         (access != VirtualCameraAccess::AllUsers ||
          lifetime == VirtualCameraLifetime::System);
}

} // namespace vividcam
