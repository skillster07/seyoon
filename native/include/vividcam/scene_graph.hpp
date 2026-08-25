#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vividcam {

enum class LayerKind { Camera, Image, Text, Color, Plugin };

enum class BlendMode { Normal, Screen, Multiply, Add };

struct LayerTransform {
  double x{0.0};
  double y{0.0};
  double width{1.0};
  double height{1.0};
  double rotation_degrees{0.0};
  double opacity{1.0};

  [[nodiscard]] bool valid() const noexcept;
};

struct LayerSource {
  LayerKind kind{LayerKind::Color};
  std::string resource;
  std::uint32_t color_rgba{0x000000FF};
};

struct SceneLayer {
  std::string id;
  std::string name;
  LayerSource source;
  LayerTransform transform;
  BlendMode blend_mode{BlendMode::Normal};
  std::int32_t z_index{0};
  bool visible{true};
  bool locked{false};
};

struct RenderCommand {
  std::string layer_id;
  LayerSource source;
  LayerTransform transform;
  BlendMode blend_mode{BlendMode::Normal};
  std::int32_t z_index{0};
};

struct SceneTemplate {
  std::string id;
  std::string name;
  std::uint32_t canvas_width{1920};
  std::uint32_t canvas_height{1080};
  std::vector<SceneLayer> layers;
};

class SceneGraph {
 public:
  SceneGraph(std::uint32_t canvas_width = 1920, std::uint32_t canvas_height = 1080);

  [[nodiscard]] bool add_layer(SceneLayer layer, std::string& error);
  [[nodiscard]] bool remove_layer(const std::string& id, std::string& error, bool force = false);
  [[nodiscard]] bool update_transform(const std::string& id, const LayerTransform& transform,
                                      std::string& error);
  [[nodiscard]] bool set_visibility(const std::string& id, bool visible, std::string& error);
  [[nodiscard]] bool set_locked(const std::string& id, bool locked, std::string& error);
  [[nodiscard]] bool move_layer(const std::string& id, std::int32_t z_index,
                                std::string& error);
  [[nodiscard]] bool apply_template(const SceneTemplate& scene_template, std::string& error);

  [[nodiscard]] const SceneLayer* find_layer(const std::string& id) const noexcept;
  [[nodiscard]] std::vector<RenderCommand> render_plan() const;
  [[nodiscard]] const std::vector<SceneLayer>& layers() const noexcept { return layers_; }
  [[nodiscard]] std::uint32_t canvas_width() const noexcept { return canvas_width_; }
  [[nodiscard]] std::uint32_t canvas_height() const noexcept { return canvas_height_; }

 private:
  SceneLayer* find_mutable(const std::string& id) noexcept;
  void normalize_z_order();

  std::uint32_t canvas_width_;
  std::uint32_t canvas_height_;
  std::vector<SceneLayer> layers_;
};

[[nodiscard]] std::vector<SceneTemplate> built_in_scene_templates();
[[nodiscard]] const char* layer_kind_name(LayerKind kind) noexcept;

} // namespace vividcam
