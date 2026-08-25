#include "vividcam/frame_compositor.hpp"

namespace vividcam {
namespace {
class UnsupportedCompositor final : public FrameCompositor {
 public:
  bool configure(const CompositorConfig&, std::string& error) override {
    error = "D3D11 frame compositor is available on Windows only";
    return false;
  }
  bool set_render_plan(const std::vector<RenderCommand>&, std::string& error) override {
    error = "D3D11 frame compositor is available on Windows only";
    return false;
  }
  [[nodiscard]] std::optional<CompositedFrame> render(
      const CapturedFrame&, std::string& error) override {
    error = "D3D11 frame compositor is available on Windows only";
    return std::nullopt;
  }
  [[nodiscard]] CompositorStatistics statistics() const override { return {}; }
  [[nodiscard]] bool valid() const noexcept override { return false; }
};
} // namespace

std::unique_ptr<FrameCompositor> create_frame_compositor(std::shared_ptr<GpuContext>) {
  return std::make_unique<UnsupportedCompositor>();
}
} // namespace vividcam
