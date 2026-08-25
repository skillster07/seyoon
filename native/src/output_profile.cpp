#include "vividcam/output_profile.hpp"

#include <sstream>

namespace vividcam {

bool OutputProfile::valid() const noexcept {
  const bool supported_size = (width == 1920 && height == 1080) ||
                              (width == 1080 && height == 1920) ||
                              (width == 1280 && height == 720) ||
                              (width == 720 && height == 1280);
  return supported_size && (frames_per_second == 30 || frames_per_second == 60) &&
         bitrate_kbps >= 2500 && bitrate_kbps <= 20000;
}

std::string OutputProfile::description() const {
  std::ostringstream stream;
  stream << platform_name(platform) << ' ' << width << 'x' << height << ' '
         << frames_per_second << "p @ " << bitrate_kbps << " Kbps";
  return stream.str();
}

OutputProfile default_profile(Platform platform) noexcept {
  switch (platform) {
    case Platform::TikTok: return {platform, 1080, 1920, 60, 6000, Encoder::Auto};
    case Platform::Obs: return {platform, 1920, 1080, 60, 8000, Encoder::Auto};
    case Platform::Soop: return {platform, 1920, 1080, 60, 8000, Encoder::Auto};
  }
  return {};
}

std::string_view platform_name(Platform platform) noexcept {
  switch (platform) {
    case Platform::Soop: return "SOOP";
    case Platform::TikTok: return "TikTok LIVE";
    case Platform::Obs: return "OBS";
  }
  return "Unknown";
}

} // namespace vividcam
