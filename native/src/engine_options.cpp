#include "vividcam/engine_options.hpp"

#include "vividcam/engine_host.hpp"

#include <charconv>
#include <cstdint>
#include <limits>

namespace vividcam {
namespace {

bool parse_positive_duration(std::string_view text, std::string_view option,
                             std::chrono::milliseconds& result,
                             std::string& error) {
  if (text.empty()) {
    error = std::string(option) + " requires a positive integer number of milliseconds";
    return false;
  }
  std::int64_t value = 0;
  const auto conversion = std::from_chars(text.data(), text.data() + text.size(), value);
  if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size() ||
      value <= 0) {
    error = std::string(option) + " requires a positive integer number of milliseconds";
    return false;
  }
  constexpr std::int64_t kMaximumRunDurationMs = 7LL * 24LL * 60LL * 60LL * 1000LL;
  if (value > kMaximumRunDurationMs) {
    error = std::string(option) + " cannot exceed 7 days";
    return false;
  }
  result = std::chrono::milliseconds{value};
  return true;
}

} // namespace

bool parse_engine_options(std::span<const std::string_view> arguments,
                          EngineOptions& options, std::string& error) {
  options = {};
  error.clear();
  bool heartbeat_seen = false;
  bool run_for_seen = false;
  bool instance_seen = false;
  bool quiet_seen = false;

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--help") {
      if (options.show_help) {
        error = "--help was provided more than once";
        return false;
      }
      options.show_help = true;
      continue;
    }
    if (argument == "--version") {
      if (options.show_version) {
        error = "--version was provided more than once";
        return false;
      }
      options.show_version = true;
      continue;
    }
    if (argument == "--quiet") {
      if (quiet_seen) {
        error = "--quiet was provided more than once";
        return false;
      }
      quiet_seen = true;
      options.quiet = true;
      continue;
    }
    if (argument == "--run-for-ms" || argument == "--heartbeat-ms" ||
        argument == "--instance-id") {
      if (index + 1 >= arguments.size()) {
        error = std::string(argument) + " requires a value";
        return false;
      }
      const std::string_view value = arguments[++index];
      if (argument == "--run-for-ms") {
        if (run_for_seen) {
          error = "--run-for-ms was provided more than once";
          return false;
        }
        run_for_seen = true;
        std::chrono::milliseconds duration{};
        if (!parse_positive_duration(value, argument, duration, error)) return false;
        options.run_for = duration;
      } else if (argument == "--heartbeat-ms") {
        if (heartbeat_seen) {
          error = "--heartbeat-ms was provided more than once";
          return false;
        }
        heartbeat_seen = true;
        std::chrono::milliseconds duration{};
        if (!parse_positive_duration(value, argument, duration, error)) return false;
        options.heartbeat_interval = duration;
      } else {
        if (instance_seen) {
          error = "--instance-id was provided more than once";
          return false;
        }
        instance_seen = true;
        const std::string instance_id{value};
        if (!valid_engine_instance_id(instance_id)) {
          error = "--instance-id must contain 1-64 ASCII letters, digits, '.', '_' or '-'";
          return false;
        }
        options.instance_id = instance_id;
      }
      continue;
    }

    error = "unknown engine option: " + std::string(argument);
    return false;
  }

  if ((options.show_help || options.show_version) && arguments.size() != 1) {
    error = "--help and --version must be used on their own";
    return false;
  }
  return true;
}

std::string engine_usage() {
  return
      "Usage: vividcam_engine [options]\n"
      "\n"
      "Runs the VIVIDCAM user-session engine host until a console signal is received.\n"
      "\n"
      "Options:\n"
      "  --run-for-ms N    Stop normally after N milliseconds (CI/smoke tests).\n"
      "  --heartbeat-ms N  Emit heartbeat telemetry every N milliseconds (default 1000).\n"
      "  --instance-id ID  Use a deterministic 1-64 character telemetry instance id.\n"
      "  --quiet           Suppress heartbeat rows; lifecycle rows are still emitted.\n"
      "  --version         Print the engine version and exit.\n"
      "  --help            Print this help and exit.\n";
}

} // namespace vividcam
