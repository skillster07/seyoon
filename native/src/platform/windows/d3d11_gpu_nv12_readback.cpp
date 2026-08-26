#include "vividcam/gpu_nv12_readback.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <limits>
#include <span>
#include <utility>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

class D3D11GpuNv12Readback final : public GpuNv12Readback {
 public:
  explicit D3D11GpuNv12Readback(std::shared_ptr<GpuContext> gpu)
      : gpu_(std::move(gpu)), latency_(600) {
    if (!gpu_ || !gpu_->valid()) return;
    auto* native_device = static_cast<ID3D11Device*>(gpu_->native_device());
    if (!native_device) return;
    device_ = native_device;
    native_device->GetImmediateContext(&context_);
  }

  bool read(const ConvertedGpuFrame& source, CpuNv12Frame& destination,
            std::string& error) override {
    const auto started = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    if (!valid()) {
      return reject("D3D11 device/context is unavailable", error);
    }
    if (source.pixel_format != VirtualCameraPixelFormat::Nv12 ||
        source.frame.width != kCpuFrameWidth ||
        source.frame.height != kCpuFrameHeight ||
        source.frame.native_texture == 0 || !source.frame.texture_owner ||
        source.frame.source_sequence == 0 ||
        source.frame.source_timestamp_100ns < 0) {
      return reject(
          "GPU readback source must be a valid 1920x1080 NV12 texture frame",
          error);
    }

    auto* source_texture =
        reinterpret_cast<ID3D11Texture2D*>(source.frame.native_texture);
    D3D11_TEXTURE2D_DESC source_description{};
    source_texture->GetDesc(&source_description);
    ComPtr<ID3D11Device> source_device;
    source_texture->GetDevice(&source_device);
    if (source_device.Get() != device_.Get() ||
        source_description.Width != kCpuFrameWidth ||
        source_description.Height != kCpuFrameHeight ||
        source_description.Format != DXGI_FORMAT_NV12 ||
        source_description.MipLevels != 1 || source_description.ArraySize != 1 ||
        source_description.SampleDesc.Count != 1) {
      return reject(
          "GPU readback texture device, format, or dimensions are invalid",
          error);
    }
    if (!ensure_staging_texture(source_description, error)) {
      ++failed_readbacks_;
      return false;
    }

    context_->CopyResource(staging_texture_.Get(), source_texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_status = context_->Map(
        staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_status)) {
      return reject("Unable to map the D3D11 NV12 staging texture", error);
    }
    if (!mapped.pData) {
      context_->Unmap(staging_texture_.Get(), 0);
      return reject("Mapped D3D11 NV12 staging texture has no data", error);
    }

    const auto row_pitch = static_cast<std::size_t>(mapped.RowPitch);
    const auto y_rows = static_cast<std::size_t>(kCpuFrameHeight);
    const auto uv_rows = y_rows / 2U;
    if (row_pitch < kCpuFrameWidth ||
        row_pitch > std::numeric_limits<std::size_t>::max() / y_rows ||
        row_pitch > std::numeric_limits<std::size_t>::max() / uv_rows) {
      context_->Unmap(staging_texture_.Get(), 0);
      return reject("Mapped D3D11 NV12 row pitch is invalid", error);
    }
    const auto y_plane_bytes = row_pitch * y_rows;
    const auto uv_plane_bytes = row_pitch * uv_rows;
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    const bool packed = pack_nv12_rows(
        kCpuFrameWidth, kCpuFrameHeight,
        std::span<const std::uint8_t>{bytes, y_plane_bytes}, row_pitch,
        std::span<const std::uint8_t>{bytes + y_plane_bytes, uv_plane_bytes},
        row_pitch, destination.bytes, error);
    context_->Unmap(staging_texture_.Get(), 0);
    if (!packed) {
      ++failed_readbacks_;
      return false;
    }

    destination.sequence = source.frame.source_sequence;
    destination.timestamp_100ns = source.frame.source_timestamp_100ns;
    destination.width = kCpuFrameWidth;
    destination.height = kCpuFrameHeight;
    destination.y_stride_bytes = kCpuFrameYStrideBytes;
    destination.uv_stride_bytes = kCpuFrameUvStrideBytes;
    if (!destination.valid()) {
      return reject("Packed D3D11 NV12 readback frame is invalid", error);
    }

    ++successful_readbacks_;
    latency_.record(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started)
                        .count());
    error.clear();
    return true;
  }

  [[nodiscard]] GpuNv12ReadbackStatistics statistics() const override {
    std::scoped_lock lock(mutex_);
    return {successful_readbacks_, failed_readbacks_, pool_allocations_,
            latency_.snapshot()};
  }

  [[nodiscard]] bool valid() const noexcept override {
    return device_ && context_;
  }

 private:
  bool reject(const char* message, std::string& error) {
    ++failed_readbacks_;
    error = message;
    return false;
  }

  bool ensure_staging_texture(const D3D11_TEXTURE2D_DESC& source,
                              std::string& error) {
    if (staging_texture_) return true;
    D3D11_TEXTURE2D_DESC description = source;
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    if (FAILED(device_->CreateTexture2D(
            &description, nullptr, &staging_texture_))) {
      error = "Unable to allocate a D3D11 NV12 staging texture";
      return false;
    }
    ++pool_allocations_;
    return true;
  }

  std::shared_ptr<GpuContext> gpu_;
  mutable std::mutex mutex_;
  std::uint64_t successful_readbacks_{0};
  std::uint64_t failed_readbacks_{0};
  std::uint64_t pool_allocations_{0};
  LatencyTracker latency_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> staging_texture_;
};

} // namespace

std::unique_ptr<GpuNv12Readback> create_gpu_nv12_readback(
    std::shared_ptr<GpuContext> gpu_context) {
  return std::make_unique<D3D11GpuNv12Readback>(std::move(gpu_context));
}

} // namespace vividcam
