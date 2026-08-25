#include "vividcam/camera_devices.hpp"

namespace vividcam {

CameraEnumerationResult enumerate_camera_devices() {
  return {{}, "Media Foundation camera enumeration is available on Windows only"};
}

} // namespace vividcam
