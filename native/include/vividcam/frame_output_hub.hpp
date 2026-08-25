#pragma once

#include "vividcam/frame_compositor.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace vividcam {

enum class OutputConsumerKind { Preview, VirtualCamera, Encoder, Ndi };

struct OutputConsumerStatistics {
  std::uint64_t published_frames{0};
  std::uint64_t consumed_frames{0};
  std::uint64_t overwritten_frames{0};
};

class FrameOutputHub {
 public:
  [[nodiscard]] bool register_consumer(std::string id, OutputConsumerKind kind,
                                       std::string& error);
  [[nodiscard]] bool unregister_consumer(const std::string& id);
  [[nodiscard]] bool publish(const CompositedFrame& frame, std::string& error);
  [[nodiscard]] std::optional<CompositedFrame> take_latest(const std::string& id);
  [[nodiscard]] std::optional<OutputConsumerStatistics> statistics(
      const std::string& id) const;
  [[nodiscard]] std::size_t consumer_count() const;

 private:
  struct Consumer {
    OutputConsumerKind kind{OutputConsumerKind::Preview};
    std::optional<CompositedFrame> latest;
    OutputConsumerStatistics statistics;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Consumer> consumers_;
};

[[nodiscard]] const char* output_consumer_kind_name(OutputConsumerKind kind) noexcept;

} // namespace vividcam
