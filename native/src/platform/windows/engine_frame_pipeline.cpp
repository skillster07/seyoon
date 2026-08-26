#include "vividcam/engine_frame_pipeline.hpp"

#include "vividcam/camera_capture.hpp"
#include "vividcam/camera_devices.hpp"
#include "vividcam/frame_compositor.hpp"
#include "vividcam/gpu_context.hpp"
#include "vividcam/gpu_nv12_readback.hpp"
#include "vividcam/gpu_pixel_converter.hpp"
#include "vividcam/output_profile.hpp"
#include "vividcam/scene_graph.hpp"
#include "vividcam/virtual_camera_media_type.hpp"

#include <algorithm>
#include <cwctype>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace vividcam {
namespace {

bool contains_vividcam(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t value) {
    return static_cast<wchar_t>(std::towupper(value));
  });
  return value.find(L"VIVIDCAM") != std::wstring::npos;
}

bool is_physical_camera(const CameraDevice& device) {
  return !device.symbolic_link.empty() &&
         !contains_vividcam(device.friendly_name) &&
         !contains_vividcam(device.symbolic_link);
}

std::optional<VirtualCameraMediaType> soop_nv12_media_type(
    const OutputProfile& profile) {
  const auto types = supported_virtual_camera_media_types(profile);
  const auto selected = std::find_if(
      types.begin(), types.end(), [](const VirtualCameraMediaType& type) {
        return type.pixel_format == VirtualCameraPixelFormat::Nv12 &&
               type.width == kCpuFrameWidth && type.height == kCpuFrameHeight &&
               type.frame_rate_numerator == 60 &&
               type.frame_rate_denominator == 1 &&
               type.stride_bytes == kCpuFrameYStrideBytes &&
               type.sample_size_bytes == kCpuFrameNv12Bytes;
      });
  return selected == types.end() ? std::nullopt
                                 : std::optional<VirtualCameraMediaType>(*selected);
}

const SceneTemplate* soop_landscape_template(
    const std::vector<SceneTemplate>& templates) {
  const auto selected = std::find_if(
      templates.begin(), templates.end(), [](const SceneTemplate& candidate) {
        return candidate.id == "soop-talk" && candidate.canvas_width == 1920 &&
               candidate.canvas_height == 1080;
      });
  return selected == templates.end() ? nullptr : &*selected;
}

} // namespace

class EngineFramePipeline::Impl {
 public:
  ~Impl() { stop(); }

  bool start(std::string& error) {
    std::scoped_lock lock(mutex_);
    reset_locked();
    snapshot_.running = true;

    try {
      initialize_gpu_locked();
      initialize_camera_locked();
      initialize_gpu_stages_locked();
      initialize_capture_locked();
      update_ready_locked();
      error.clear();
      return true;
    } catch (const std::exception& exception) {
      snapshot_.pipeline_error =
          std::string("Engine frame pipeline initialization failed: ") +
          exception.what();
    } catch (...) {
      snapshot_.pipeline_error =
          "Engine frame pipeline initialization failed unexpectedly";
    }

    error = snapshot_.pipeline_error;
    reset_resources_locked();
    snapshot_.running = false;
    snapshot_.ready = false;
    return false;
  }

  void stop() noexcept {
    std::scoped_lock lock(mutex_);
    reset_resources_locked();
    snapshot_.running = false;
    snapshot_.ready = false;
  }

  bool running() const noexcept {
    std::scoped_lock lock(mutex_);
    return snapshot_.running;
  }

  bool take_latest_cpu_frame(CpuNv12Frame& output, std::string& error) {
    std::scoped_lock lock(mutex_);
    ++snapshot_.poll_attempts;
    update_ready_locked();

    if (!snapshot_.running) {
      return no_frame_locked("Engine frame pipeline is not running", error);
    }
    if (!snapshot_.ready) {
      return no_frame_locked(degraded_error_locked(), error);
    }

    if (auto captured = capture_->take_latest_frame()) {
      if (!captured->gpu || captured->gpu->native_texture == 0 ||
          !captured->gpu->sample_owner) {
        ++snapshot_.cpu_only_frames;
        snapshot_.capture_error =
            "Camera capture produced a CPU-only frame; GPU input is required";
        latest_gpu_input_.reset();
        return no_frame_locked(snapshot_.capture_error, error);
      }
      latest_gpu_input_ = std::move(*captured);
      ++snapshot_.new_capture_frames;
      snapshot_.capture_error.clear();
    } else if (latest_gpu_input_) {
      ++snapshot_.repeated_capture_frames;
    } else {
      return no_frame_locked(
          "Camera capture has not produced a GPU-backed frame", error);
    }

    std::string stage_error;
    auto composited = compositor_->render(*latest_gpu_input_, stage_error);
    if (!composited) {
      ++snapshot_.render_failures;
      snapshot_.compositor_error = stage_error.empty()
                                       ? "Frame compositor returned no frame"
                                       : std::move(stage_error);
      return no_frame_locked(snapshot_.compositor_error, error);
    }
    ++snapshot_.rendered_frames;
    snapshot_.compositor_error.clear();

    stage_error.clear();
    auto converted = converter_->convert(*composited, stage_error);
    if (!converted) {
      ++snapshot_.conversion_failures;
      snapshot_.conversion_error = stage_error.empty()
                                       ? "GPU NV12 converter returned no frame"
                                       : std::move(stage_error);
      return no_frame_locked(snapshot_.conversion_error, error);
    }
    ++snapshot_.converted_frames;
    snapshot_.conversion_error.clear();

    stage_error.clear();
    if (!readback_->read(*converted, output, stage_error)) {
      ++snapshot_.readback_failures;
      snapshot_.readback_error = stage_error.empty()
                                     ? "GPU NV12 readback returned no frame"
                                     : std::move(stage_error);
      return no_frame_locked(snapshot_.readback_error, error);
    }
    // The readback contract copies these fields from the composited source.
    // Assign them explicitly as a defense against an implementation that only
    // copies texture bytes, and to keep repeated-input detection stable.
    output.sequence = latest_gpu_input_->sequence;
    output.timestamp_100ns = latest_gpu_input_->timestamp_100ns;
    if (!output.valid()) {
      ++snapshot_.readback_failures;
      snapshot_.readback_error =
          "GPU NV12 readback returned an invalid frame";
      return no_frame_locked(snapshot_.readback_error, error);
    }
    ++snapshot_.readback_frames;
    snapshot_.readback_error.clear();
    error.clear();
    return true;
  }

  EngineFramePipelineSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    EngineFramePipelineSnapshot result = snapshot_;
    if (readback_) {
      result.readback_latency = readback_->statistics().readback_latency;
    }
    if (result.running && capture_ && !capture_->running()) result.ready = false;
    return result;
  }

 private:
  void initialize_gpu_locked() {
    auto result = create_gpu_context(true);
    if (!result.succeeded()) {
      snapshot_.gpu_error = result.error.empty()
                                ? "No usable D3D11 GPU context is available"
                                : std::move(result.error);
      return;
    }
    gpu_ = std::move(result.context);
    snapshot_.gpu_error.clear();
  }

  void initialize_camera_locked() {
    const auto cameras = enumerate_camera_devices();
    if (!cameras.supported()) {
      snapshot_.camera_error = cameras.error.empty()
                                   ? "Camera enumeration is unavailable"
                                   : cameras.error;
      return;
    }
    const bool has_physical_camera = std::any_of(
        cameras.devices.begin(), cameras.devices.end(), is_physical_camera);
    if (!has_physical_camera) {
      snapshot_.camera_error =
          "No non-VIVIDCAM physical input camera is available";
      return;
    }

    for (const auto& device : cameras.devices) {
      if (!is_physical_camera(device)) continue;
      auto format = select_preferred_gpu_compositor_format(
          device.formats, 1920, 1080, 60);
      if (!format) continue;
      camera_symbolic_link_ = device.symbolic_link;
      camera_format_ = std::move(format);
      snapshot_.camera_error.clear();
      return;
    }
    snapshot_.camera_error =
        "No non-VIVIDCAM camera has an uncompressed GPU compositor capture "
        "format";
  }

  void initialize_gpu_stages_locked() {
    if (!gpu_) return;

    const OutputProfile profile = default_profile(Platform::Soop);
    const CompositorConfig config{
        profile.width, profile.height, profile.frames_per_second,
        CanvasOrientation::Landscape};

    compositor_ = create_frame_compositor(gpu_);
    std::string stage_error;
    if (!compositor_ || !compositor_->valid() ||
        !compositor_->configure(config, stage_error)) {
      snapshot_.compositor_error = stage_error.empty()
                                       ? "D3D11 frame compositor is unavailable"
                                       : std::move(stage_error);
      compositor_.reset();
    } else {
      const auto templates = built_in_scene_templates();
      const auto* selected_template = soop_landscape_template(templates);
      if (!selected_template) {
        snapshot_.scene_error = "SOOP landscape scene template is unavailable";
      } else {
        SceneGraph scene(selected_template->canvas_width,
                         selected_template->canvas_height);
        if (!scene.apply_template(*selected_template, stage_error) ||
            !compositor_->set_render_plan(scene.render_plan(), stage_error)) {
          snapshot_.scene_error = stage_error.empty()
                                      ? "SOOP landscape render plan is invalid"
                                      : std::move(stage_error);
        } else {
          snapshot_.scene_error.clear();
          snapshot_.compositor_error.clear();
        }
      }
    }

    converter_ = create_gpu_pixel_converter(gpu_);
    const auto nv12_type = soop_nv12_media_type(profile);
    stage_error.clear();
    if (!nv12_type) {
      snapshot_.conversion_error =
          "Fixed 1920x1080 NV12 60p output type is unavailable";
      converter_.reset();
    } else if (!converter_ || !converter_->valid() ||
               !converter_->configure(*nv12_type, stage_error)) {
      snapshot_.conversion_error = stage_error.empty()
                                       ? "D3D11 NV12 converter is unavailable"
                                       : std::move(stage_error);
      converter_.reset();
    } else {
      snapshot_.conversion_error.clear();
    }

    readback_ = create_gpu_nv12_readback(gpu_);
    if (!readback_ || !readback_->valid()) {
      snapshot_.readback_error = "D3D11 NV12 readback is unavailable";
      readback_.reset();
    } else {
      snapshot_.readback_error.clear();
    }
  }

  void initialize_capture_locked() {
    if (!camera_format_ || camera_symbolic_link_.empty()) return;
    if (!gpu_ || !compositor_ || !snapshot_.scene_error.empty() || !converter_ ||
        !readback_) {
      snapshot_.capture_error =
          "Camera capture is waiting for the complete GPU frame pipeline";
      return;
    }

    capture_ = create_camera_capture_session();
    std::string capture_error;
    const CaptureOptions options{true, gpu_};
    if (!capture_ || !capture_->start(camera_symbolic_link_, *camera_format_,
                                      options, capture_error)) {
      snapshot_.capture_error = capture_error.empty()
                                    ? "Unable to start physical camera capture"
                                    : std::move(capture_error);
      capture_.reset();
      return;
    }
    snapshot_.capture_error.clear();
  }

  void update_ready_locked() {
    snapshot_.ready = snapshot_.running && gpu_ && camera_format_.has_value() &&
                      capture_ && capture_->running() && compositor_ &&
                      snapshot_.scene_error.empty() && converter_ && readback_;
    if (snapshot_.running && capture_ && !capture_->running() &&
        snapshot_.capture_error.empty()) {
      snapshot_.capture_error = "Physical camera capture stopped";
    }
  }

  bool no_frame_locked(const std::string& reason, std::string& error) {
    ++snapshot_.no_frame_polls;
    error = reason.empty() ? "Engine frame pipeline has no frame" : reason;
    return false;
  }

  std::string degraded_error_locked() const {
    for (const auto* candidate : {
             &snapshot_.pipeline_error, &snapshot_.gpu_error,
             &snapshot_.camera_error, &snapshot_.capture_error,
             &snapshot_.scene_error, &snapshot_.compositor_error,
             &snapshot_.conversion_error, &snapshot_.readback_error}) {
      if (!candidate->empty()) return *candidate;
    }
    return "Engine frame pipeline is not ready";
  }

  void reset_locked() noexcept {
    reset_resources_locked();
    snapshot_ = {};
  }

  void reset_resources_locked() noexcept {
    if (readback_) {
      try {
        snapshot_.readback_latency =
            readback_->statistics().readback_latency;
      } catch (...) {
        // Resource cleanup must remain noexcept. Preserve the most recent
        // successfully collected latency snapshot if statistics allocation
        // fails during shutdown.
      }
    }
    // Captured samples own both an IMF sample and its D3D11 texture. Release
    // the pipeline's retained sample before the capture session shuts down MF.
    latest_gpu_input_.reset();
    if (capture_) capture_->stop();
    capture_.reset();
    readback_.reset();
    converter_.reset();
    compositor_.reset();
    gpu_.reset();
    camera_symbolic_link_.clear();
    camera_format_.reset();
  }

  mutable std::mutex mutex_;
  EngineFramePipelineSnapshot snapshot_;
  std::shared_ptr<GpuContext> gpu_;
  std::unique_ptr<CameraCaptureSession> capture_;
  std::unique_ptr<FrameCompositor> compositor_;
  std::unique_ptr<GpuPixelConverter> converter_;
  std::unique_ptr<GpuNv12Readback> readback_;
  std::wstring camera_symbolic_link_;
  std::optional<CameraFormat> camera_format_;
  std::optional<CapturedFrame> latest_gpu_input_;
};

EngineFramePipeline::EngineFramePipeline() : impl_(std::make_unique<Impl>()) {}
EngineFramePipeline::~EngineFramePipeline() = default;

bool EngineFramePipeline::start(std::string& error) {
  return impl_->start(error);
}

void EngineFramePipeline::stop() noexcept { impl_->stop(); }

bool EngineFramePipeline::running() const noexcept { return impl_->running(); }

bool EngineFramePipeline::take_latest_cpu_frame(CpuNv12Frame& output,
                                                std::string& error) {
  return impl_->take_latest_cpu_frame(output, error);
}

EngineFramePipelineSnapshot EngineFramePipeline::snapshot() const {
  return impl_->snapshot();
}

} // namespace vividcam
