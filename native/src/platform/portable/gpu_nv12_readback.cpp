#include "vividcam/gpu_nv12_readback.hpp"

#include <mutex>

namespace vividcam {
namespace {

class UnsupportedGpuNv12Readback final : public GpuNv12Readback {
 public:
  bool read(const ConvertedGpuFrame&, CpuNv12Frame&,
            std::string& error) override {
    std::scoped_lock lock(mutex_);
    ++failed_readbacks_;
    error = "D3D11 GPU NV12 readback is available on Windows only";
    return false;
  }

  [[nodiscard]] GpuNv12ReadbackStatistics statistics() const override {
    std::scoped_lock lock(mutex_);
    return {0, failed_readbacks_, 0, {}};
  }

  [[nodiscard]] bool valid() const noexcept override { return false; }

 private:
  mutable std::mutex mutex_;
  std::uint64_t failed_readbacks_{0};
};

} // namespace

std::unique_ptr<GpuNv12Readback> create_gpu_nv12_readback(
    std::shared_ptr<GpuContext>) {
  return std::make_unique<UnsupportedGpuNv12Readback>();
}

} // namespace vividcam
