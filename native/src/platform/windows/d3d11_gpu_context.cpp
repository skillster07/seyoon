#include "vividcam/gpu_context.hpp"

#include <Windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <memory>
#include <string>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

std::string adapter_name(ID3D11Device* device) {
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC description{};
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
      FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter->GetDesc(&description))) {
    return "Unknown D3D11 adapter";
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
                                          nullptr, 0, nullptr, nullptr);
  if (length <= 1) return "Unknown D3D11 adapter";
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, description.Description, -1,
                      result.data(), length, nullptr, nullptr);
  result.pop_back();
  return result;
}

class D3D11GpuContext final : public GpuContext {
 public:
  bool initialize(bool allow_software_fallback, std::string& error) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    constexpr D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL selected_level{};

    HRESULT status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       requested_levels, ARRAYSIZE(requested_levels),
                                       D3D11_SDK_VERSION, &device_, &selected_level, &context_);
    backend_ = GpuBackend::D3D11Hardware;
    if (FAILED(status) && allow_software_fallback) {
      status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                 requested_levels, ARRAYSIZE(requested_levels),
                                 D3D11_SDK_VERSION, &device_, &selected_level, &context_);
      backend_ = GpuBackend::D3D11Warp;
    }
    if (FAILED(status)) {
      error = "D3D11CreateDevice failed";
      return false;
    }

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    status = MFCreateDXGIDeviceManager(&reset_token_, &device_manager_);
    if (SUCCEEDED(status)) status = device_manager_->ResetDevice(device_.Get(), reset_token_);
    if (FAILED(status)) {
      error = "MFCreateDXGIDeviceManager or ResetDevice failed";
      return false;
    }
    adapter_name_ = adapter_name(device_.Get());
    return true;
  }

  [[nodiscard]] bool valid() const noexcept override {
    return device_ && context_ && device_manager_;
  }
  [[nodiscard]] GpuDeviceInfo info() const override {
    return {backend_, adapter_name_, valid()};
  }
  [[nodiscard]] void* native_device() const noexcept override { return device_.Get(); }
  [[nodiscard]] void* native_device_manager() const noexcept override {
    return device_manager_.Get();
  }
  [[nodiscard]] std::uint32_t reset_token() const noexcept override { return reset_token_; }

 private:
  GpuBackend backend_{GpuBackend::Unsupported};
  std::string adapter_name_;
  UINT reset_token_{0};
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
};
} // namespace

GpuContextResult create_gpu_context(bool allow_software_fallback) {
  auto context = std::make_shared<D3D11GpuContext>();
  std::string error;
  if (!context->initialize(allow_software_fallback, error)) return {nullptr, std::move(error)};
  return {std::move(context), {}};
}

} // namespace vividcam
