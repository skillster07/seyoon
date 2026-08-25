#include "vividcam/latency_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace vividcam {

LatencyTracker::LatencyTracker(std::size_t capacity) : capacity_(capacity) {
  if (capacity == 0) throw std::invalid_argument("latency capacity must be positive");
}

void LatencyTracker::record(double milliseconds) {
  if (!std::isfinite(milliseconds) || milliseconds < 0.0) return;
  std::scoped_lock lock(mutex_);
  if (samples_.size() == capacity_) samples_.pop_front();
  samples_.push_back(milliseconds);
}

void LatencyTracker::reset() {
  std::scoped_lock lock(mutex_);
  samples_.clear();
}

LatencySnapshot LatencyTracker::snapshot() const {
  std::scoped_lock lock(mutex_);
  if (samples_.empty()) return {};
  std::vector<double> sorted(samples_.begin(), samples_.end());
  std::sort(sorted.begin(), sorted.end());
  const auto percentile = [&](double value) {
    const auto index = static_cast<std::size_t>(
        std::ceil(value * static_cast<double>(sorted.size())) - 1.0);
    return sorted[std::min(index, sorted.size() - 1)];
  };
  const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
  return {sorted.size(), percentile(0.50), percentile(0.95), sorted.back(),
          sum / static_cast<double>(sorted.size())};
}

} // namespace vividcam
