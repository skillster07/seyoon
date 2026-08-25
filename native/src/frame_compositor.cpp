#include "vividcam/frame_compositor.hpp"

namespace vividcam {

bool valid_compositor_config(const CompositorConfig& config) noexcept {
  const bool size = (config.width == 1920 && config.height == 1080) ||
                    (config.width == 1080 && config.height == 1920) ||
                    (config.width == 1280 && config.height == 720) ||
                    (config.width == 720 && config.height == 1280);
  return size && (config.frames_per_second == 30 || config.frames_per_second == 60);
}

bool valid_render_plan(const std::vector<RenderCommand>& commands) noexcept {
  std::size_t camera_count = 0;
  for (const auto& command : commands) {
    if (command.layer_id.empty() || !command.transform.valid()) return false;
    if (command.source.kind == LayerKind::Camera) ++camera_count;
  }
  return camera_count <= 1;
}

} // namespace vividcam
