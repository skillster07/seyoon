#pragma once

#include "vividcam/scene_graph.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vividcam {

struct ImageResource {
  std::string id;
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba;

  [[nodiscard]] bool valid() const noexcept;
};

struct TextResource {
  std::string id;
  std::string text;
  std::string font_family{"sans-serif"};
  double font_size{48.0};
  std::uint32_t color_rgba{0xFFFFFFFF};

  [[nodiscard]] bool valid() const noexcept;
};

class LayerResourceStore {
 public:
  [[nodiscard]] bool put_image(ImageResource resource, std::string& error);
  [[nodiscard]] bool put_text(TextResource resource, std::string& error);
  [[nodiscard]] bool remove(const std::string& id);
  [[nodiscard]] const ImageResource* image(const std::string& id) const noexcept;
  [[nodiscard]] const TextResource* text(const std::string& id) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  std::unordered_map<std::string, ImageResource> images_;
  std::unordered_map<std::string, TextResource> texts_;
};

struct ReferenceCanvas {
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::vector<std::uint8_t> rgba;
};

struct ReferenceRenderResult {
  ReferenceCanvas canvas;
  std::uint64_t rendered_layers{0};
  std::uint64_t skipped_layers{0};
};

[[nodiscard]] std::optional<ReferenceRenderResult> render_reference_scene(
    std::uint32_t width, std::uint32_t height,
    const std::vector<RenderCommand>& commands,
    const LayerResourceStore& resources, std::string& error);

} // namespace vividcam
