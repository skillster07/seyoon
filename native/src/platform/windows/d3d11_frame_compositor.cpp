#include "vividcam/frame_compositor.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace vividcam {
namespace {
using Microsoft::WRL::ComPtr;

struct OutputSlot {
  ComPtr<ID3D11Texture2D> texture;
};

class D3D11FrameCompositor final : public FrameCompositor {
 public:
  explicit D3D11FrameCompositor(std::shared_ptr<GpuContext> gpu)
      : gpu_(std::move(gpu)), latency_(600) {
    if (!gpu_ || !gpu_->valid()) return;
    auto* native_device = static_cast<ID3D11Device*>(gpu_->native_device());
    device_ = native_device;
    native_device->GetImmediateContext(&device_context_);
    device_.As(&video_device_);
    device_context_.As(&video_context_);
  }

  bool configure(const CompositorConfig& config, std::string& error) override {
    if (!valid()) {
      error = "D3D11 video device/context is unavailable";
      return false;
    }
    if (!valid_compositor_config(config)) {
      error = "Unsupported compositor output profile";
      return false;
    }
    std::scoped_lock lock(mutex_);
    config_ = config;
    configured_ = true;
    reset_processor();
    latency_.reset();
    rendered_frames_ = 0;
    rejected_frames_ = 0;
    pool_allocations_ = 0;
    skipped_layers_ = 0;
    camera_transform_ = {};
    background_color_ = 0x000000FF;
    return true;
  }

  bool set_render_plan(const std::vector<RenderCommand>& commands,
                       std::string& error) override {
    if (!valid_render_plan(commands)) {
      error = "Render plan is invalid or contains multiple camera layers";
      return false;
    }
    std::scoped_lock lock(mutex_);
    camera_transform_.reset();
    skipped_layers_ = 0;
    bool found_background = false;
    for (const auto& command : commands) {
      if (command.source.kind == LayerKind::Color && !found_background) {
        background_color_ = command.source.color_rgba;
        found_background = true;
      } else if (command.source.kind == LayerKind::Camera) {
        camera_transform_ = command.transform;
      } else {
        ++skipped_layers_;
      }
    }
    return true;
  }

  [[nodiscard]] std::optional<CompositedFrame> render(
      const CapturedFrame& source, std::string& error) override {
    const auto started = std::chrono::steady_clock::now();
    std::scoped_lock lock(mutex_);
    if (!configured_ || !source.gpu || source.gpu->native_texture == 0) {
      ++rejected_frames_;
      error = "Compositor requires a configured GPU-backed source frame";
      return std::nullopt;
    }

    auto* source_texture = reinterpret_cast<ID3D11Texture2D*>(source.gpu->native_texture);
    D3D11_TEXTURE2D_DESC source_desc{};
    source_texture->GetDesc(&source_desc);
    if (!ensure_processor(source, source_desc, error)) {
      ++rejected_frames_;
      return std::nullopt;
    }

    const auto slot = acquire_output_slot(error);
    if (!slot) {
      ++rejected_frames_;
      return std::nullopt;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.FourCC = 0;
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.MipSlice = 0;
    input_desc.Texture2D.ArraySlice = source_desc.MipLevels == 0
                                          ? 0
                                          : source.gpu->subresource_index / source_desc.MipLevels;
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT status = video_device_->CreateVideoProcessorInputView(
        source_texture, processor_enumerator_.Get(), &input_desc, &input_view);

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_desc.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    if (SUCCEEDED(status)) status = video_device_->CreateVideoProcessorOutputView(
        slot->texture.Get(), processor_enumerator_.Get(), &output_desc, &output_view);

    const RECT source_rect{0, 0, static_cast<LONG>(source_desc.Width),
                           static_cast<LONG>(source_desc.Height)};
    const RECT output_rect{0, 0, static_cast<LONG>(config_.width),
                           static_cast<LONG>(config_.height)};
    const auto transform = camera_transform_.value_or(LayerTransform{});
    const auto pixel = [](double value, std::uint32_t extent) {
      return static_cast<LONG>(value * static_cast<double>(extent));
    };
    const RECT camera_rect{pixel(transform.x, config_.width),
                           pixel(transform.y, config_.height),
                           pixel(transform.x + transform.width, config_.width),
                           pixel(transform.y + transform.height, config_.height)};
    if (SUCCEEDED(status)) {
      video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source_rect);
      video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &camera_rect);
      video_context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &output_rect);
      video_context_->VideoProcessorSetStreamAlpha(processor_.Get(), 0, TRUE,
                                                    static_cast<FLOAT>(transform.opacity));
      D3D11_VIDEO_COLOR background{};
      background.RGBA.R = static_cast<float>((background_color_ >> 24) & 0xFF) / 255.0F;
      background.RGBA.G = static_cast<float>((background_color_ >> 16) & 0xFF) / 255.0F;
      background.RGBA.B = static_cast<float>((background_color_ >> 8) & 0xFF) / 255.0F;
      background.RGBA.A = static_cast<float>(background_color_ & 0xFF) / 255.0F;
      video_context_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE,
                                                              &background);

      ComPtr<ID3D11VideoContext1> video_context1;
      if (config_.orientation == CanvasOrientation::Portrait &&
          SUCCEEDED(video_context_.As(&video_context1))) {
        video_context1->VideoProcessorSetStreamRotation(
            processor_.Get(), 0, TRUE, D3D11_VIDEO_PROCESSOR_ROTATION_90);
      } else if (SUCCEEDED(video_context_.As(&video_context1))) {
        video_context1->VideoProcessorSetStreamRotation(
            processor_.Get(), 0, FALSE, D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY);
      }

      D3D11_VIDEO_PROCESSOR_STREAM stream{};
      stream.Enable = camera_transform_.has_value();
      stream.pInputSurface = input_view.Get();
      status = video_context_->VideoProcessorBlt(processor_.Get(), output_view.Get(),
                                                  0, stream.Enable ? 1 : 0, &stream);
    }
    if (FAILED(status)) {
      ++rejected_frames_;
      error = "D3D11 VideoProcessorBlt failed";
      return std::nullopt;
    }

    ++rendered_frames_;
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    latency_.record(elapsed);
    return CompositedFrame{source.sequence, source.timestamp_100ns, config_.width,
                           config_.height, slot,
                           reinterpret_cast<std::uintptr_t>(slot->texture.Get())};
  }

  [[nodiscard]] CompositorStatistics statistics() const override {
    std::scoped_lock lock(mutex_);
    return {rendered_frames_, rejected_frames_, pool_allocations_, skipped_layers_,
            latency_.snapshot()};
  }

  [[nodiscard]] bool valid() const noexcept override {
    return device_ && device_context_ && video_device_ && video_context_;
  }

 private:
  bool ensure_processor(const CapturedFrame& source, const D3D11_TEXTURE2D_DESC& source_desc,
                        std::string& error) {
    if (processor_ && input_width_ == source_desc.Width && input_height_ == source_desc.Height &&
        input_format_ == source_desc.Format) return true;
    reset_processor();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};
    description.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    description.InputFrameRate.Numerator = source.format.frames_per_second_numerator;
    description.InputFrameRate.Denominator = source.format.frames_per_second_denominator;
    description.InputWidth = source_desc.Width;
    description.InputHeight = source_desc.Height;
    description.OutputFrameRate.Numerator = config_.frames_per_second;
    description.OutputFrameRate.Denominator = 1;
    description.OutputWidth = config_.width;
    description.OutputHeight = config_.height;
    description.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT status = video_device_->CreateVideoProcessorEnumerator(
        &description, &processor_enumerator_);
    if (SUCCEEDED(status)) status = video_device_->CreateVideoProcessor(
        processor_enumerator_.Get(), 0, &processor_);
    if (FAILED(status)) {
      error = "Unable to create D3D11 video processor";
      reset_processor();
      return false;
    }
    input_width_ = source_desc.Width;
    input_height_ = source_desc.Height;
    input_format_ = source_desc.Format;
    return true;
  }

  std::shared_ptr<OutputSlot> acquire_output_slot(std::string& error) {
    for (const auto& slot : output_pool_) {
      if (slot.use_count() == 1) return slot;
    }
    if (output_pool_.size() >= 8) {
      error = "Compositor output pool exhausted";
      return nullptr;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = config_.width;
    description.Height = config_.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = 0;

    auto slot = std::make_shared<OutputSlot>();
    if (FAILED(device_->CreateTexture2D(&description, nullptr, &slot->texture))) {
      error = "Unable to allocate compositor output texture";
      return nullptr;
    }
    output_pool_.push_back(slot);
    ++pool_allocations_;
    return slot;
  }

  void reset_processor() {
    processor_.Reset();
    processor_enumerator_.Reset();
    output_pool_.clear();
    input_width_ = 0;
    input_height_ = 0;
    input_format_ = DXGI_FORMAT_UNKNOWN;
  }

  std::shared_ptr<GpuContext> gpu_;
  mutable std::mutex mutex_;
  CompositorConfig config_{};
  bool configured_{false};
  std::uint32_t input_width_{0};
  std::uint32_t input_height_{0};
  DXGI_FORMAT input_format_{DXGI_FORMAT_UNKNOWN};
  std::uint64_t rendered_frames_{0};
  std::uint64_t rejected_frames_{0};
  std::uint64_t pool_allocations_{0};
  std::uint64_t skipped_layers_{0};
  std::optional<LayerTransform> camera_transform_;
  std::uint32_t background_color_{0x000000FF};
  LatencyTracker latency_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> device_context_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> processor_enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  std::vector<std::shared_ptr<OutputSlot>> output_pool_;
};
} // namespace

std::unique_ptr<FrameCompositor> create_frame_compositor(
    std::shared_ptr<GpuContext> gpu_context) {
  return std::make_unique<D3D11FrameCompositor>(std::move(gpu_context));
}
} // namespace vividcam
