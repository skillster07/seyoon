#include "vividcam/scene_graph.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace vividcam {
namespace {

bool valid_layer(const SceneLayer& layer) {
  return !layer.id.empty() && !layer.name.empty() && layer.transform.valid();
}

SceneLayer make_layer(std::string id, std::string name, LayerKind kind,
                      std::string resource, std::uint32_t color, LayerTransform transform,
                      std::int32_t z, bool locked = false) {
  return {std::move(id), std::move(name), {kind, std::move(resource), color}, transform,
          BlendMode::Normal, z, true, locked};
}
} // namespace

bool LayerTransform::valid() const noexcept {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
         std::isfinite(height) && std::isfinite(rotation_degrees) &&
         std::isfinite(opacity) && width > 0.0 && height > 0.0 &&
         x >= 0.0 && y >= 0.0 && x + width <= 1.0 && y + height <= 1.0 &&
         opacity >= 0.0 && opacity <= 1.0;
}

SceneGraph::SceneGraph(std::uint32_t canvas_width, std::uint32_t canvas_height)
    : canvas_width_(canvas_width), canvas_height_(canvas_height) {
  if (canvas_width_ == 0) canvas_width_ = 1920;
  if (canvas_height_ == 0) canvas_height_ = 1080;
}

bool SceneGraph::add_layer(SceneLayer layer, std::string& error) {
  if (!valid_layer(layer)) {
    error = "Layer id, name, or transform is invalid";
    return false;
  }
  if (find_layer(layer.id)) {
    error = "Layer id already exists";
    return false;
  }
  layers_.push_back(std::move(layer));
  normalize_z_order();
  return true;
}

bool SceneGraph::remove_layer(const std::string& id, std::string& error, bool force) {
  const auto found = std::find_if(layers_.begin(), layers_.end(),
                                 [&](const SceneLayer& layer) { return layer.id == id; });
  if (found == layers_.end()) {
    error = "Layer not found";
    return false;
  }
  if (found->locked && !force) {
    error = "Locked layer cannot be removed";
    return false;
  }
  layers_.erase(found);
  normalize_z_order();
  return true;
}

bool SceneGraph::update_transform(const std::string& id, const LayerTransform& transform,
                                  std::string& error) {
  auto* layer = find_mutable(id);
  if (!layer) {
    error = "Layer not found";
    return false;
  }
  if (layer->locked) {
    error = "Locked layer cannot be transformed";
    return false;
  }
  if (!transform.valid()) {
    error = "Layer transform is invalid";
    return false;
  }
  layer->transform = transform;
  return true;
}

bool SceneGraph::set_visibility(const std::string& id, bool visible, std::string& error) {
  auto* layer = find_mutable(id);
  if (!layer) {
    error = "Layer not found";
    return false;
  }
  layer->visible = visible;
  return true;
}

bool SceneGraph::set_locked(const std::string& id, bool locked, std::string& error) {
  auto* layer = find_mutable(id);
  if (!layer) {
    error = "Layer not found";
    return false;
  }
  layer->locked = locked;
  return true;
}

bool SceneGraph::move_layer(const std::string& id, std::int32_t z_index, std::string& error) {
  auto* layer = find_mutable(id);
  if (!layer) {
    error = "Layer not found";
    return false;
  }
  if (layer->locked) {
    error = "Locked layer cannot be reordered";
    return false;
  }
  layer->z_index = z_index;
  normalize_z_order();
  return true;
}

bool SceneGraph::apply_template(const SceneTemplate& scene_template, std::string& error) {
  if (scene_template.id.empty() || scene_template.name.empty() ||
      scene_template.canvas_width == 0 || scene_template.canvas_height == 0) {
    error = "Template metadata is invalid";
    return false;
  }
  std::unordered_set<std::string> ids;
  for (const auto& layer : scene_template.layers) {
    if (!valid_layer(layer) || !ids.insert(layer.id).second) {
      error = "Template contains an invalid or duplicate layer";
      return false;
    }
  }
  canvas_width_ = scene_template.canvas_width;
  canvas_height_ = scene_template.canvas_height;
  layers_ = scene_template.layers;
  normalize_z_order();
  return true;
}

const SceneLayer* SceneGraph::find_layer(const std::string& id) const noexcept {
  const auto found = std::find_if(layers_.begin(), layers_.end(),
                                 [&](const SceneLayer& layer) { return layer.id == id; });
  return found == layers_.end() ? nullptr : &*found;
}

SceneLayer* SceneGraph::find_mutable(const std::string& id) noexcept {
  const auto found = std::find_if(layers_.begin(), layers_.end(),
                                 [&](const SceneLayer& layer) { return layer.id == id; });
  return found == layers_.end() ? nullptr : &*found;
}

std::vector<RenderCommand> SceneGraph::render_plan() const {
  std::vector<RenderCommand> commands;
  commands.reserve(layers_.size());
  for (const auto& layer : layers_) {
    if (!layer.visible || layer.transform.opacity <= 0.0) continue;
    commands.push_back({layer.id, layer.source, layer.transform, layer.blend_mode,
                        layer.z_index});
  }
  std::stable_sort(commands.begin(), commands.end(),
                   [](const RenderCommand& left, const RenderCommand& right) {
                     return left.z_index < right.z_index;
                   });
  return commands;
}

void SceneGraph::normalize_z_order() {
  std::stable_sort(layers_.begin(), layers_.end(),
                   [](const SceneLayer& left, const SceneLayer& right) {
                     return left.z_index < right.z_index;
                   });
  for (std::size_t index = 0; index < layers_.size(); ++index) {
    layers_[index].z_index = static_cast<std::int32_t>(index);
  }
}

std::vector<SceneTemplate> built_in_scene_templates() {
  const LayerTransform full{0.0, 0.0, 1.0, 1.0, 0.0, 1.0};
  return {
      {"soop-talk", "SOOP 토크 60p", 1920, 1080,
       {make_layer("background", "스튜디오 배경", LayerKind::Color, "", 0x171522FF,
                   full, 0, true),
        make_layer("camera", "메인 카메라", LayerKind::Camera, "camera:primary", 0,
                   {0.18, 0.05, 0.64, 0.90, 0.0, 1.0}, 1),
        make_layer("title", "방송 제목", LayerKind::Text, "오늘도 반가워요", 0xFFFFFFFF,
                   {0.04, 0.84, 0.42, 0.10, 0.0, 1.0}, 2)}},
      {"tiktok-portrait", "TikTok 세로 60p", 1080, 1920,
       {make_layer("background", "세로 배경", LayerKind::Color, "", 0x21182EFF,
                   full, 0, true),
        make_layer("camera", "세로 카메라", LayerKind::Camera, "camera:primary", 0,
                   {0.0, 0.08, 1.0, 0.72, 0.0, 1.0}, 1),
        make_layer("safe-title", "안전 영역 제목", LayerKind::Text, "LIVE", 0xFFFFFFFF,
                   {0.06, 0.05, 0.35, 0.06, 0.0, 1.0}, 2)}},
      {"starting-soon", "잠시 후 시작", 1920, 1080,
       {make_layer("background", "대기 배경", LayerKind::Color, "", 0x171522FF,
                   full, 0, true),
        make_layer("message", "대기 메시지", LayerKind::Text,
                   "잠시 후 방송을 시작합니다", 0xFFFFFFFF,
                   {0.20, 0.40, 0.60, 0.20, 0.0, 1.0}, 1)}}};
}

const char* layer_kind_name(LayerKind kind) noexcept {
  switch (kind) {
    case LayerKind::Camera: return "Camera";
    case LayerKind::Image: return "Image";
    case LayerKind::Text: return "Text";
    case LayerKind::Color: return "Color";
    case LayerKind::Plugin: return "Plugin";
  }
  return "Unknown";
}

} // namespace vividcam
