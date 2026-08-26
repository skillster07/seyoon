#include "vividcam/gpu_nv12_readback.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

std::uint8_t expected_y(std::size_t row, std::size_t column) {
  return static_cast<std::uint8_t>((row * 3U + column * 5U) & 0xffU);
}

std::uint8_t expected_uv(std::size_t row, std::size_t column) {
  return static_cast<std::uint8_t>((row * 7U + column * 11U + 13U) & 0xffU);
}

void verify_packed_pattern(const vividcam::CpuNv12Frame& frame) {
  assert(frame.bytes.size() == vividcam::kCpuFrameNv12Bytes);
  const auto width = static_cast<std::size_t>(vividcam::kCpuFrameWidth);
  const auto height = static_cast<std::size_t>(vividcam::kCpuFrameHeight);
  for (std::size_t row = 0; row < height; ++row) {
    for (std::size_t column = 0; column < width; ++column) {
      assert(frame.bytes[row * width + column] == expected_y(row, column));
    }
  }
  const std::size_t uv_offset = width * height;
  for (std::size_t row = 0; row < height / 2U; ++row) {
    for (std::size_t column = 0; column < width; ++column) {
      assert(frame.bytes[uv_offset + row * width + column] ==
             expected_uv(row, column));
    }
  }
}

void test_row_packing() {
  constexpr std::size_t kYRowPitch = vividcam::kCpuFrameWidth + 32U;
  constexpr std::size_t kUvRowPitch = vividcam::kCpuFrameWidth + 64U;
  constexpr std::size_t kYRows = vividcam::kCpuFrameHeight;
  constexpr std::size_t kUvRows = vividcam::kCpuFrameHeight / 2U;
  std::vector<std::uint8_t> y_plane(kYRowPitch * kYRows, 0xeeU);
  std::vector<std::uint8_t> uv_plane(kUvRowPitch * kUvRows, 0xddU);
  for (std::size_t row = 0; row < kYRows; ++row) {
    for (std::size_t column = 0; column < vividcam::kCpuFrameWidth; ++column) {
      y_plane[row * kYRowPitch + column] = expected_y(row, column);
    }
  }
  for (std::size_t row = 0; row < kUvRows; ++row) {
    for (std::size_t column = 0; column < vividcam::kCpuFrameWidth; ++column) {
      uv_plane[row * kUvRowPitch + column] = expected_uv(row, column);
    }
  }

  vividcam::CpuNv12Frame packed;
  std::string error;
  assert(vividcam::pack_nv12_rows(
      vividcam::kCpuFrameWidth, vividcam::kCpuFrameHeight,
      std::span<const std::uint8_t>{y_plane}, kYRowPitch,
      std::span<const std::uint8_t>{uv_plane}, kUvRowPitch, packed.bytes,
      error));
  assert(error.empty());
  verify_packed_pattern(packed);

  const auto* allocation = packed.bytes.data();
  assert(vividcam::pack_nv12_rows(
      vividcam::kCpuFrameWidth, vividcam::kCpuFrameHeight,
      std::span<const std::uint8_t>{y_plane}, kYRowPitch,
      std::span<const std::uint8_t>{uv_plane}, kUvRowPitch, packed.bytes,
      error));
  assert(packed.bytes.data() == allocation);

  const auto size_before_rejection = packed.bytes.size();
  const auto first_before_rejection = packed.bytes.front();
  assert(!vividcam::pack_nv12_rows(
      vividcam::kCpuFrameWidth - 2U, vividcam::kCpuFrameHeight,
      std::span<const std::uint8_t>{y_plane}, kYRowPitch,
      std::span<const std::uint8_t>{uv_plane}, kUvRowPitch, packed.bytes,
      error));
  assert(!error.empty() && packed.bytes.size() == size_before_rejection &&
         packed.bytes.front() == first_before_rejection);
  assert(!vividcam::pack_nv12_rows(
      vividcam::kCpuFrameWidth, vividcam::kCpuFrameHeight,
      std::span<const std::uint8_t>{y_plane},
      vividcam::kCpuFrameWidth - 1U,
      std::span<const std::uint8_t>{uv_plane}, kUvRowPitch, packed.bytes,
      error));
  assert(!vividcam::pack_nv12_rows(
      vividcam::kCpuFrameWidth, vividcam::kCpuFrameHeight,
      std::span<const std::uint8_t>{y_plane}.first(y_plane.size() - 1U),
      kYRowPitch, std::span<const std::uint8_t>{uv_plane}, kUvRowPitch,
      packed.bytes, error));
  assert(!vividcam::pack_nv12_rows(
      vividcam::kCpuFrameWidth, vividcam::kCpuFrameHeight,
      std::span<const std::uint8_t>{y_plane}, kYRowPitch,
      std::span<const std::uint8_t>{uv_plane}.first(uv_plane.size() - 1U),
      kUvRowPitch, packed.bytes, error));
}

void test_invalid_reader() {
  auto reader = vividcam::create_gpu_nv12_readback(nullptr);
  assert(reader && !reader->valid());
  vividcam::ConvertedGpuFrame source;
  vividcam::CpuNv12Frame destination;
  std::string error;
  assert(!reader->read(source, destination, error));
  assert(!error.empty());
  const auto statistics = reader->statistics();
  assert(statistics.successful_readbacks == 0);
  assert(statistics.failed_readbacks == 1);
  assert(statistics.pool_allocations == 0);
  assert(statistics.readback_latency.samples == 0);
}

#ifdef _WIN32
void test_d3d11_readback_when_available() {
  using Microsoft::WRL::ComPtr;
  const auto gpu = vividcam::create_gpu_context(true);
  if (!gpu.succeeded()) {
    std::cout << "D3D11 NV12 readback smoke skipped: " << gpu.error << '\n';
    return;
  }
  auto reader = vividcam::create_gpu_nv12_readback(gpu.context);
  assert(reader && reader->valid());
  auto* device = static_cast<ID3D11Device*>(gpu.context->native_device());
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);

  D3D11_TEXTURE2D_DESC description{};
  description.Width = vividcam::kCpuFrameWidth;
  description.Height = vividcam::kCpuFrameHeight;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_NV12;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_STAGING;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&description, nullptr, &texture))) {
    std::cout << "D3D11 NV12 readback smoke skipped: texture unsupported\n";
    return;
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  const HRESULT map_status =
      context->Map(texture.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
  if (FAILED(map_status) || !mapped.pData ||
      mapped.RowPitch < vividcam::kCpuFrameWidth) {
    if (SUCCEEDED(map_status)) context->Unmap(texture.Get(), 0);
    std::cout << "D3D11 NV12 readback smoke skipped: texture map unsupported\n";
    return;
  }
  auto* bytes = static_cast<std::uint8_t*>(mapped.pData);
  const auto row_pitch = static_cast<std::size_t>(mapped.RowPitch);
  for (std::size_t row = 0; row < vividcam::kCpuFrameHeight; ++row) {
    for (std::size_t column = 0; column < vividcam::kCpuFrameWidth; ++column) {
      bytes[row * row_pitch + column] = expected_y(row, column);
    }
  }
  auto* uv_plane = bytes + row_pitch * vividcam::kCpuFrameHeight;
  for (std::size_t row = 0; row < vividcam::kCpuFrameHeight / 2U; ++row) {
    for (std::size_t column = 0; column < vividcam::kCpuFrameWidth; ++column) {
      uv_plane[row * row_pitch + column] = expected_uv(row, column);
    }
  }
  context->Unmap(texture.Get(), 0);

  auto* raw_texture = texture.Detach();
  std::shared_ptr<void> owner(raw_texture, [](void* value) {
    static_cast<ID3D11Texture2D*>(value)->Release();
  });
  vividcam::CompositedFrame frame{
      42, 1'234'567, vividcam::kCpuFrameWidth, vividcam::kCpuFrameHeight,
      owner, reinterpret_cast<std::uintptr_t>(raw_texture)};
  vividcam::ConvertedGpuFrame source{
      std::move(frame), vividcam::VirtualCameraPixelFormat::Nv12};
  vividcam::CpuNv12Frame destination;
  std::string error;
  assert(reader->read(source, destination, error));
  assert(error.empty() && destination.valid());
  assert(destination.sequence == 42);
  assert(destination.timestamp_100ns == 1'234'567);
  verify_packed_pattern(destination);

  const auto* allocation = destination.bytes.data();
  assert(reader->read(source, destination, error));
  assert(destination.bytes.data() == allocation);
  const auto statistics = reader->statistics();
  assert(statistics.successful_readbacks == 2);
  assert(statistics.failed_readbacks == 0);
  assert(statistics.pool_allocations == 1);
  assert(statistics.readback_latency.samples == 2);
}
#endif

} // namespace

int main() {
  test_row_packing();
  test_invalid_reader();
#ifdef _WIN32
  test_d3d11_readback_when_available();
#endif
  std::cout << "VIVIDCAM GPU NV12 readback tests passed\n";
  return 0;
}
