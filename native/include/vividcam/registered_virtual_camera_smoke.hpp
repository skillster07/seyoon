#pragma once

#include <cstdint>
#include <string>

namespace vividcam {

struct RegisteredVirtualCameraSmokeResult {
  bool supported{false};
  bool passed{false};
  std::uint32_t samples{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t fps_numerator{0};
  std::uint32_t fps_denominator{1};
  std::uint32_t distinct_checksums{0};
  std::uint32_t empty_callbacks{0};
  std::uint32_t source_reader_flags{0};
  std::int64_t first_timestamp_100ns{0};
  std::int64_t last_timestamp_100ns{0};
  std::int64_t average_timestamp_delta_100ns{0};
  std::int64_t minimum_timestamp_delta_100ns{0};
  std::int64_t maximum_timestamp_delta_100ns{0};
  std::int64_t minimum_duration_100ns{0};
  std::int64_t maximum_duration_100ns{0};
  std::string error;
};

[[nodiscard]] RegisteredVirtualCameraSmokeResult
run_registered_virtual_camera_smoke(std::uint32_t required_samples = 12,
                                    std::uint32_t timeout_ms = 15000);

} // namespace vividcam
