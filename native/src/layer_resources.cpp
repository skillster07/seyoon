#include "vividcam/layer_resources.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace vividcam {
namespace {
constexpr std::uint64_t kMaximumReferencePixels = 3840ULL * 2160ULL;

void blend_pixel(std::uint8_t* destination, const std::uint8_t* source, double opacity) {
  const auto source_alpha = static_cast<std::uint32_t>(std::lround(
      static_cast<double>(source[3]) * std::clamp(opacity, 0.0, 1.0)));
  const auto inverse_alpha = 255U - source_alpha;
  for (std::size_t channel = 0; channel < 3; ++channel) {
    destination[channel] = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(source[channel]) * source_alpha +
         static_cast<std::uint32_t>(destination[channel]) * inverse_alpha + 127U) / 255U);
  }
  destination[3] = static_cast<std::uint8_t>(
      source_alpha + (static_cast<std::uint32_t>(destination[3]) * inverse_alpha + 127U) / 255U);
}

std::array<std::uint8_t, 4> unpack_color(std::uint32_t rgba) {
  return {static_cast<std::uint8_t>((rgba >> 24) & 0xFF),
          static_cast<std::uint8_t>((rgba >> 16) & 0xFF),
          static_cast<std::uint8_t>((rgba >> 8) & 0xFF),
          static_cast<std::uint8_t>(rgba & 0xFF)};
}
} // namespace

bool ImageResource::valid() const noexcept {
  if (id.empty() || width == 0 || height == 0) return false;
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  return pixels <= kMaximumReferencePixels && rgba.size() == pixels * 4ULL;
}

bool TextResource::valid() const noexcept {
  return !id.empty() && !text.empty() && !font_family.empty() &&
         std::isfinite(font_size) && font_size >= 8.0 && font_size <= 512.0;
}

bool LayerResourceStore::put_image(ImageResource resource, std::string& error) {
  if (!resource.valid()) {
    error = "Image resource id, dimensions, or RGBA byte size is invalid";
    return false;
  }
  texts_.erase(resource.id);
  images_.insert_or_assign(resource.id, std::move(resource));
  return true;
}

bool LayerResourceStore::put_text(TextResource resource, std::string& error) {
  if (!resource.valid()) {
    error = "Text resource id, content, font, or size is invalid";
    return false;
  }
  images_.erase(resource.id);
  texts_.insert_or_assign(resource.id, std::move(resource));
  return true;
}

bool LayerResourceStore::remove(const std::string& id) {
  return images_.erase(id) + texts_.erase(id) > 0;
}

const ImageResource* LayerResourceStore::image(const std::string& id) const noexcept {
  const auto found = images_.find(id);
  return found == images_.end() ? nullptr : &found->second;
}

const TextResource* LayerResourceStore::text(const std::string& id) const noexcept {
  const auto found = texts_.find(id);
  return found == texts_.end() ? nullptr : &found->second;
}

std::size_t LayerResourceStore::size() const noexcept {
  return images_.size() + texts_.size();
}

std::optional<ReferenceRenderResult> render_reference_scene(
    std::uint32_t width, std::uint32_t height,
    const std::vector<RenderCommand>& commands,
    const LayerResourceStore& resources, std::string& error) {
  const auto pixel_count = static_cast<std::uint64_t>(width) * height;
  if (width == 0 || height == 0 || pixel_count > kMaximumReferencePixels) {
    error = "Reference canvas dimensions are invalid or exceed the safety limit";
    return std::nullopt;
  }
  ReferenceRenderResult result{{width, height,
                                std::vector<std::uint8_t>(pixel_count * 4ULL, 0)}, 0, 0};
  for (const auto& command : commands) {
    if (!command.transform.valid() || command.blend_mode != BlendMode::Normal) {
      ++result.skipped_layers;
      continue;
    }
    const auto left = static_cast<std::uint32_t>(std::lround(command.transform.x * width));
    const auto top = static_cast<std::uint32_t>(std::lround(command.transform.y * height));
    const auto right = static_cast<std::uint32_t>(
        std::lround((command.transform.x + command.transform.width) * width));
    const auto bottom = static_cast<std::uint32_t>(
        std::lround((command.transform.y + command.transform.height) * height));
    const auto destination_width = right - left;
    const auto destination_height = bottom - top;
    if (destination_width == 0 || destination_height == 0) {
      ++result.skipped_layers;
      continue;
    }
    const ImageResource* image = nullptr;
    std::array<std::uint8_t, 4> color{};
    if (command.source.kind == LayerKind::Color) {
      color = unpack_color(command.source.color_rgba);
    } else if (command.source.kind == LayerKind::Image) {
      image = resources.image(command.source.resource);
      if (!image) {
        error = "Image layer references a missing resource: " + command.source.resource;
        return std::nullopt;
      }
    } else {
      ++result.skipped_layers;
      continue;
    }
    for (std::uint32_t y = top; y < bottom; ++y) {
      for (std::uint32_t x = left; x < right; ++x) {
        const std::uint8_t* source = color.data();
        if (image) {
          const auto source_x = (x - left) * image->width / destination_width;
          const auto source_y = (y - top) * image->height / destination_height;
          source = &image->rgba[(static_cast<std::size_t>(source_y) * image->width + source_x) * 4];
        }
        auto* destination = &result.canvas.rgba[
            (static_cast<std::size_t>(y) * width + x) * 4];
        blend_pixel(destination, source, command.transform.opacity);
      }
    }
    ++result.rendered_layers;
  }
  return result;
}

} // namespace vividcam
