#include "vividcam/frame_output_hub.hpp"

#include <utility>

namespace vividcam {

bool FrameOutputHub::register_consumer(std::string id, OutputConsumerKind kind,
                                       std::string& error) {
  if (id.empty()) {
    error = "Output consumer id cannot be empty";
    return false;
  }
  std::scoped_lock lock(mutex_);
  const auto result = consumers_.emplace(std::move(id), Consumer{kind, {}, {}});
  const bool inserted = result.second;
  if (!inserted) error = "Output consumer id already exists";
  return inserted;
}

bool FrameOutputHub::unregister_consumer(const std::string& id) {
  std::scoped_lock lock(mutex_);
  return consumers_.erase(id) == 1;
}

bool FrameOutputHub::publish(const CompositedFrame& frame, std::string& error) {
  if (frame.width == 0 || frame.height == 0 || frame.native_texture == 0 ||
      !frame.texture_owner) {
    error = "Output frame must own a valid GPU texture";
    return false;
  }
  std::scoped_lock lock(mutex_);
  for (auto& entry : consumers_) {
    auto& consumer = entry.second;
    if (consumer.latest) ++consumer.statistics.overwritten_frames;
    consumer.latest = frame;
    ++consumer.statistics.published_frames;
  }
  return true;
}

std::optional<CompositedFrame> FrameOutputHub::take_latest(const std::string& id) {
  std::scoped_lock lock(mutex_);
  const auto found = consumers_.find(id);
  if (found == consumers_.end() || !found->second.latest) return std::nullopt;
  auto frame = std::move(found->second.latest);
  found->second.latest.reset();
  ++found->second.statistics.consumed_frames;
  return frame;
}

std::optional<OutputConsumerStatistics> FrameOutputHub::statistics(
    const std::string& id) const {
  std::scoped_lock lock(mutex_);
  const auto found = consumers_.find(id);
  if (found == consumers_.end()) return std::nullopt;
  return found->second.statistics;
}

std::size_t FrameOutputHub::consumer_count() const {
  std::scoped_lock lock(mutex_);
  return consumers_.size();
}

const char* output_consumer_kind_name(OutputConsumerKind kind) noexcept {
  switch (kind) {
    case OutputConsumerKind::Preview: return "Preview";
    case OutputConsumerKind::VirtualCamera: return "VirtualCamera";
    case OutputConsumerKind::Encoder: return "Encoder";
    case OutputConsumerKind::Ndi: return "NDI";
  }
  return "Unknown";
}

} // namespace vividcam
