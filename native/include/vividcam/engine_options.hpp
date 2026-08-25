#pragma once

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vividcam {

inline constexpr std::string_view kEngineVersion = "0.1.0";

struct EngineOptions {
  std::chrono::milliseconds heartbeat_interval{std::chrono::seconds{1}};
  std::optional<std::chrono::milliseconds> run_for;
  std::optional<std::string> instance_id;
  bool quiet{false};
  bool show_help{false};
  bool show_version{false};
};

bool parse_engine_options(std::span<const std::string_view> arguments,
                          EngineOptions& options, std::string& error);
[[nodiscard]] std::string engine_usage();

} // namespace vividcam
