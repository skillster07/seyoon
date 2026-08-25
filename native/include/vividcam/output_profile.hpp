#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vividcam {

enum class Platform { Soop, TikTok, Obs };
enum class Encoder { Auto, Nvenc, QuickSync, Amf };

struct OutputProfile {
  Platform platform{Platform::Soop};
  std::uint32_t width{1920};
  std::uint32_t height{1080};
  std::uint32_t frames_per_second{60};
  std::uint32_t bitrate_kbps{8000};
  Encoder encoder{Encoder::Auto};

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool portrait() const noexcept { return height > width; }
  [[nodiscard]] std::string description() const;
};

[[nodiscard]] OutputProfile default_profile(Platform platform) noexcept;
[[nodiscard]] std::string_view platform_name(Platform platform) noexcept;

} // namespace vividcam
