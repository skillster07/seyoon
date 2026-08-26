#include "vividcam/engine_frame_pipeline.hpp"

#include <mutex>

namespace vividcam {

class EngineFramePipeline::Impl {
 public:
  bool start(std::string& error) {
    std::scoped_lock lock(mutex_);
    snapshot_ = {};
    snapshot_.running = true;
    snapshot_.ready = false;
    snapshot_.gpu_error =
        "The engine GPU frame pipeline is available on Windows only";
    error.clear();
    return true;
  }

  void stop() noexcept {
    std::scoped_lock lock(mutex_);
    snapshot_.running = false;
    snapshot_.ready = false;
  }

  bool running() const noexcept {
    std::scoped_lock lock(mutex_);
    return snapshot_.running;
  }

  bool take_latest_cpu_frame(CpuNv12Frame&, std::string& error) {
    std::scoped_lock lock(mutex_);
    ++snapshot_.poll_attempts;
    ++snapshot_.no_frame_polls;
    error = snapshot_.running
                ? snapshot_.gpu_error
                : "Engine frame pipeline is not running";
    return false;
  }

  EngineFramePipelineSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
  }

 private:
  mutable std::mutex mutex_;
  EngineFramePipelineSnapshot snapshot_;
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
