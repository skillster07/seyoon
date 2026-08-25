#pragma once

#include "vividcam/camera_devices.hpp"
#include "vividcam/gpu_context.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vividcam {

struct GpuFrameHandle {
  std::shared_ptr<void> sample_owner;
  std::uintptr_t native_texture{0};
  std::uint32_t subresource_index{0};
};

struct CapturedFrame {
  std::uint64_t sequence{0};
  std::int64_t timestamp_100ns{0};
  std::int64_t duration_100ns{0};
  CameraFormat format;
  std::vector<std::uint8_t> bytes;
  std::optional<GpuFrameHandle> gpu;
};

struct CaptureStatistics {
  std::uint64_t received_frames{0};
  std::uint64_t consumed_frames{0};
  std::uint64_t overwritten_frames{0};
  std::uint64_t source_errors{0};
  std::uint64_t gpu_frames{0};
  std::uint64_t cpu_frames{0};
};

struct CaptureOptions {
  bool prefer_gpu_surfaces{true};
  std::shared_ptr<GpuContext> gpu_context;
};

class CameraCaptureSession {
 public:
  virtual ~CameraCaptureSession() = default;
  virtual bool start(const std::wstring& symbolic_link, const CameraFormat& format,
                     const CaptureOptions& options, std::string& error) = 0;
  virtual void stop() noexcept = 0;
  [[nodiscard]] virtual bool running() const noexcept = 0;
  [[nodiscard]] virtual std::optional<CapturedFrame> take_latest_frame() = 0;
  [[nodiscard]] virtual CaptureStatistics statistics() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<CameraCaptureSession> create_camera_capture_session();

} // namespace vividcam
