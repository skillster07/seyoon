#pragma once

#include <cstddef>
#include <deque>
#include <mutex>

namespace vividcam {

struct LatencySnapshot {
  std::size_t samples{0};
  double p50_ms{0.0};
  double p95_ms{0.0};
  double max_ms{0.0};
  double average_ms{0.0};
};

class LatencyTracker {
 public:
  explicit LatencyTracker(std::size_t capacity = 600);
  void record(double milliseconds);
  void reset();
  [[nodiscard]] LatencySnapshot snapshot() const;

 private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<double> samples_;
};

} // namespace vividcam
