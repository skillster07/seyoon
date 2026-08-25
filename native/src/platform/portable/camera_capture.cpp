#include "vividcam/camera_capture.hpp"

namespace vividcam {
namespace {
class UnsupportedCaptureSession final : public CameraCaptureSession {
 public:
  bool start(const std::wstring&, const CameraFormat&, const CaptureOptions&, std::string& error) override {
    error = "Media Foundation capture is available on Windows only";
    return false;
  }
  void stop() noexcept override {}
  [[nodiscard]] bool running() const noexcept override { return false; }
  [[nodiscard]] std::optional<CapturedFrame> take_latest_frame() override { return std::nullopt; }
  [[nodiscard]] CaptureStatistics statistics() const noexcept override { return {}; }
};
} // namespace

std::unique_ptr<CameraCaptureSession> create_camera_capture_session() {
  return std::make_unique<UnsupportedCaptureSession>();
}
} // namespace vividcam
