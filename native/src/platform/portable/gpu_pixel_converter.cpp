#include "vividcam/gpu_pixel_converter.hpp"

namespace vividcam {
namespace {
class UnsupportedGpuPixelConverter final : public GpuPixelConverter {
 public:
  bool configure(const VirtualCameraMediaType&, std::string& error) override {
    error = "D3D11 GPU pixel conversion is available on Windows only";
    return false;
  }
  [[nodiscard]] std::optional<ConvertedGpuFrame> convert(
      const CompositedFrame&, std::string& error) override {
    error = "D3D11 GPU pixel conversion is available on Windows only";
    return std::nullopt;
  }
  [[nodiscard]] GpuPixelConverterStatistics statistics() const override { return {}; }
  [[nodiscard]] bool valid() const noexcept override { return false; }
};
} // namespace

std::unique_ptr<GpuPixelConverter> create_gpu_pixel_converter(std::shared_ptr<GpuContext>) {
  return std::make_unique<UnsupportedGpuPixelConverter>();
}

} // namespace vividcam
