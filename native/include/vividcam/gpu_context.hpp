#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace vividcam {

enum class GpuBackend { Unsupported, D3D11Hardware, D3D11Warp };

struct GpuDeviceInfo {
  GpuBackend backend{GpuBackend::Unsupported};
  std::string adapter_name;
  bool video_support{false};
};

class GpuContext {
 public:
  virtual ~GpuContext() = default;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual GpuDeviceInfo info() const = 0;
  [[nodiscard]] virtual void* native_device() const noexcept = 0;
  [[nodiscard]] virtual void* native_device_manager() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t reset_token() const noexcept = 0;
};

struct GpuContextResult {
  std::shared_ptr<GpuContext> context;
  std::string error;
  [[nodiscard]] bool succeeded() const noexcept { return context && context->valid(); }
};

[[nodiscard]] GpuContextResult create_gpu_context(bool allow_software_fallback = true);
[[nodiscard]] const char* gpu_backend_name(GpuBackend backend) noexcept;

} // namespace vividcam
