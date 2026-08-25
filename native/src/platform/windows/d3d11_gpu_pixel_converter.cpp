#include "vividcam/gpu_pixel_converter.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

struct ConversionSlot {
  ComPtr<ID3D11Texture2D> texture;
};

class D3D11GpuPixelConverter final : public GpuPixelConverter {
 public:
  explicit D3D11GpuPixelConverter(std::shared_ptr<GpuContext> gpu)
      : gpu_(std::move(gpu)), latency_(600) {
    if (!gpu_ || !gpu_->valid()) return;
    auto* native_device = static_cast<ID3D11Device*>(gpu_->native_device());
    device_ = native_device;
    native_device->GetImmediateContext(&context_);
    device_.As(&video_device_);
    context_.As(&video_context_);
  }

  bool configure(const VirtualCameraMediaType& output_type,
                 std::string& error) override {
    if (!valid()) {
      error = "D3D11 video device/context is unavailable";
      return false;
    }
    if (!valid_gpu_conversion_output(output_type)) {
      error = "GPU converter currently requires a valid NV12 output type";
      return false;
    }
    std::scoped_lock lock(mutex_);
    output_type_ = output_type;
    configured_ = true;
    reset_resources();
    converted_frames_ = 0;
    rejected_frames_ = 0;
    pool_allocations_ = 0;
    latency_.reset();
    return true;
  }

  [[nodiscard]] std::optional<ConvertedGpuFrame> convert(
      const CompositedFrame& source, std::string& error) override {
    const auto started = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    if (!configured_ || source.native_texture == 0 || !source.texture_owner ||
        source.width != output_type_.width || source.height != output_type_.height) {
      ++rejected_frames_;
      error = "GPU conversion source is invalid or does not match the output dimensions";
      return std::nullopt;
    }
    auto* source_texture = reinterpret_cast<ID3D11Texture2D*>(source.native_texture);
    D3D11_TEXTURE2D_DESC source_description{};
    source_texture->GetDesc(&source_description);
    if (!ensure_processor(source_description, error)) {
      ++rejected_frames_;
      return std::nullopt;
    }
    const auto slot = acquire_slot(error);
    if (!slot) {
      ++rejected_frames_;
      return std::nullopt;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_description{};
    input_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_description.Texture2D.MipSlice = 0;
    input_description.Texture2D.ArraySlice = 0;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT status = video_device_->CreateVideoProcessorInputView(
        source_texture, enumerator_.Get(), &input_description, &input_view);

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_description{};
    output_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_description.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    if (SUCCEEDED(status)) {
      status = video_device_->CreateVideoProcessorOutputView(
          slot->texture.Get(), enumerator_.Get(), &output_description, &output_view);
    }

    const RECT rectangle{0, 0, static_cast<LONG>(output_type_.width),
                         static_cast<LONG>(output_type_.height)};
    if (SUCCEEDED(status)) {
      video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &rectangle);
      video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &rectangle);
      video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &rectangle);
      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = TRUE;
      stream.pInputSurface = input_view.Get();
      status = video_context_->VideoProcessorBlt(
          processor_.Get(), output_view.Get(), 0, 1, &stream);
    }
    if (FAILED(status)) {
      ++rejected_frames_;
      error = "D3D11 VideoProcessor BGRA-to-NV12 conversion failed";
      return std::nullopt;
    }

    ++converted_frames_;
    latency_.record(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - started).count());
    CompositedFrame converted{source.source_sequence, source.source_timestamp_100ns,
                              output_type_.width, output_type_.height, slot,
                              reinterpret_cast<std::uintptr_t>(slot->texture.Get())};
    return ConvertedGpuFrame{std::move(converted), VirtualCameraPixelFormat::Nv12};
  }

  [[nodiscard]] GpuPixelConverterStatistics statistics() const override {
    std::scoped_lock lock(mutex_);
    return {converted_frames_, rejected_frames_, pool_allocations_, latency_.snapshot()};
  }

  [[nodiscard]] bool valid() const noexcept override {
    return device_ && context_ && video_device_ && video_context_;
  }

 private:
  bool ensure_processor(const D3D11_TEXTURE2D_DESC& source, std::string& error) {
    if (processor_ && source_format_ == source.Format) return true;
    reset_resources();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {output_type_.frame_rate_numerator,
                              output_type_.frame_rate_denominator};
    content.InputWidth = source.Width;
    content.InputHeight = source.Height;
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = output_type_.width;
    content.OutputHeight = output_type_.height;
    content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    HRESULT status = video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_);
    if (SUCCEEDED(status)) {
      status = video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
    }
    if (FAILED(status)) {
      error = "Unable to create D3D11 BGRA-to-NV12 video processor";
      reset_resources();
      return false;
    }
    source_format_ = source.Format;
    return true;
  }

  std::shared_ptr<ConversionSlot> acquire_slot(std::string& error) {
    for (const auto& slot : pool_) {
      if (slot.use_count() == 1) return slot;
    }
    if (pool_.size() >= 8) {
      error = "GPU conversion output pool exhausted";
      return nullptr;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = output_type_.width;
    description.Height = output_type_.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = 0;
    auto slot = std::make_shared<ConversionSlot>();
    if (FAILED(device_->CreateTexture2D(&description, nullptr, &slot->texture))) {
      error = "Unable to allocate shared D3D11 NV12 texture";
      return nullptr;
    }
    pool_.push_back(slot);
    ++pool_allocations_;
    return slot;
  }

  void reset_resources() {
    processor_.Reset();
    enumerator_.Reset();
    pool_.clear();
    source_format_ = DXGI_FORMAT_UNKNOWN;
  }

  std::shared_ptr<GpuContext> gpu_;
  mutable std::mutex mutex_;
  VirtualCameraMediaType output_type_{};
  bool configured_{false};
  std::uint64_t converted_frames_{0};
  std::uint64_t rejected_frames_{0};
  std::uint64_t pool_allocations_{0};
  LatencyTracker latency_;
  DXGI_FORMAT source_format_{DXGI_FORMAT_UNKNOWN};
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  std::vector<std::shared_ptr<ConversionSlot>> pool_;
};
} // namespace

std::unique_ptr<GpuPixelConverter> create_gpu_pixel_converter(
    std::shared_ptr<GpuContext> gpu_context) {
  return std::make_unique<D3D11GpuPixelConverter>(std::move(gpu_context));
}

} // namespace vividcam
