#pragma once

#include "vividcam/frame_compositor.hpp"
#include "vividcam/output_profile.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace vividcam {

struct VirtualCameraSample {
  CompositedFrame frame;
  std::int64_t timestamp_100ns{0};
  std::int64_t duration_100ns{0};
  bool repeated{false};
};

struct VirtualCameraStatistics {
  std::uint64_t submitted_frames{0};
  std::uint64_t delivered_samples{0};
  std::uint64_t repeated_samples{0};
  std::uint64_t overwritten_frames{0};
  std::uint64_t underruns{0};
  std::uint64_t rejected_frames{0};
};

class VirtualCameraStream {
 public:
  explicit VirtualCameraStream(OutputProfile profile = default_profile(Platform::Soop));

  [[nodiscard]] bool configure(const OutputProfile& profile, std::string& error);
  [[nodiscard]] bool submit(CompositedFrame frame, std::string& error);
  [[nodiscard]] std::optional<VirtualCameraSample> request_sample();
  void reset();

  [[nodiscard]] OutputProfile profile() const;
  [[nodiscard]] VirtualCameraStatistics statistics() const;

 private:
  [[nodiscard]] bool matches_profile(const CompositedFrame& frame) const noexcept;

  mutable std::mutex mutex_;
  OutputProfile profile_;
  std::optional<CompositedFrame> pending_;
  std::optional<CompositedFrame> last_delivered_;
  std::uint64_t sample_index_{0};
  VirtualCameraStatistics statistics_;
};

} // namespace vividcam
