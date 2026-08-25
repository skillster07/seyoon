#include "vividcam/engine_host.hpp"
#include "vividcam/engine_options.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

int main() {
  using namespace std::chrono_literals;
  using vividcam::EngineHost;
  using vividcam::EngineLifecycleState;
  using vividcam::EngineStopReason;
  using vividcam::EngineTickResult;

  assert(std::string(vividcam::engine_lifecycle_state_name(
             EngineLifecycleState::Running)) == "running");
  assert(std::string(vividcam::engine_stop_reason_name(
             EngineStopReason::RunForElapsed)) == "run-for");
  assert(vividcam::valid_engine_instance_id("engine-1.test_case"));
  assert(!vividcam::valid_engine_instance_id(""));
  assert(!vividcam::valid_engine_instance_id("engine with spaces"));

  bool invalid_instance_rejected = false;
  try {
    EngineHost invalid("bad instance");
  } catch (const std::invalid_argument&) {
    invalid_instance_rejected = true;
  }
  assert(invalid_instance_rejected);

  bool invalid_interval_rejected = false;
  try {
    EngineHost invalid("engine", 0ms);
  } catch (const std::invalid_argument&) {
    invalid_interval_rejected = true;
  }
  assert(invalid_interval_rejected);

  const EngineHost::TimePoint epoch{};
  EngineHost host("unit-test", 1s);
  std::string error;
  assert(host.snapshot(epoch).state == EngineLifecycleState::Created);
  assert(host.begin_start(epoch, error));
  assert(host.snapshot(epoch).state == EngineLifecycleState::Starting);
  assert(host.mark_running(epoch, error));
  assert(host.snapshot(epoch).state == EngineLifecycleState::Running);
  assert(host.next_heartbeat_deadline() == epoch + 1s);

  assert(host.tick(epoch + 999ms, error) == EngineTickResult::NoHeartbeat);
  assert(host.tick(epoch + 1s, error) == EngineTickResult::Heartbeat);
  auto status = host.snapshot(epoch + 1s);
  assert(status.schema_version == vividcam::kEngineTelemetrySchemaVersion);
  assert(status.instance_id == "unit-test");
  assert(status.heartbeat_sequence == 1);
  assert(status.missed_heartbeat_intervals == 0);
  assert(status.uptime_ms == 1000);
  assert(status.last_heartbeat_uptime_ms == 1000);
  assert(!status.frame_transport_ready);

  assert(host.tick(epoch + 3500ms, error) == EngineTickResult::Heartbeat);
  status = host.snapshot(epoch + 3500ms);
  assert(status.heartbeat_sequence == 2);
  assert(status.missed_heartbeat_intervals == 1);
  assert(status.last_heartbeat_uptime_ms == 3500);
  assert(host.next_heartbeat_deadline() == epoch + 4s);

  assert(host.tick(epoch + 3400ms, error) == EngineTickResult::Rejected);
  assert(!error.empty());
  assert(host.request_stop(EngineStopReason::Requested, epoch + 3600ms, error));
  assert(host.request_stop(EngineStopReason::ConsoleSignal, epoch + 3700ms, error));
  status = host.snapshot(epoch + 3700ms);
  assert(status.state == EngineLifecycleState::Stopping);
  assert(status.stop_reason == EngineStopReason::Requested);
  assert(host.tick(epoch + 3700ms, error) == EngineTickResult::Rejected);
  assert(host.snapshot(epoch + 3700ms).heartbeat_sequence == 2);
  assert(host.mark_stopped(epoch + 3800ms, error));
  assert(host.mark_stopped(epoch + 3900ms, error));
  status = host.snapshot(epoch + 10s);
  assert(status.state == EngineLifecycleState::Stopped);
  assert(status.uptime_ms == 3800);
  assert(!host.next_heartbeat_deadline());
  assert(!host.begin_start(epoch + 11s, error));

  EngineHost monotonic_snapshot("monotonic-snapshot");
  assert(monotonic_snapshot.begin_start(epoch, error));
  assert(monotonic_snapshot.mark_running(epoch, error));
  assert(monotonic_snapshot.snapshot(epoch + 10s).uptime_ms == 10'000);
  assert(monotonic_snapshot.snapshot(epoch + 1s).uptime_ms == 10'000);
  assert(!monotonic_snapshot.request_stop(
      EngineStopReason::Requested, epoch + 2s, error));
  assert(monotonic_snapshot.request_stop(
      EngineStopReason::Requested, epoch + 11s, error));

  EngineHost failed("failed-test");
  assert(failed.mark_failed("startup failure", epoch + 1s, error));
  status = failed.snapshot(epoch + 10s);
  assert(status.state == EngineLifecycleState::Failed);
  assert(status.stop_reason == EngineStopReason::Failure);
  assert(status.failure == "startup failure");
  assert(status.uptime_ms == 0);
  assert(failed.mark_failed("ignored failure", epoch + 2s, error));
  assert(failed.snapshot(epoch + 2s).failure == "startup failure");

  EngineHost invalid_stop("invalid-stop");
  assert(!invalid_stop.request_stop(EngineStopReason::Requested, epoch, error));
  assert(invalid_stop.begin_start(epoch, error));
  assert(invalid_stop.mark_running(epoch, error));
  assert(!invalid_stop.request_stop(EngineStopReason::None, epoch, error));
  assert(!invalid_stop.request_stop(EngineStopReason::Failure, epoch, error));

  vividcam::EngineOptions options;
  const std::vector<std::string_view> no_arguments;
  assert(vividcam::parse_engine_options(no_arguments, options, error));
  assert(options.heartbeat_interval == 1s);
  assert(!options.run_for && !options.instance_id && !options.quiet);

  const std::array configured_arguments = {
      std::string_view{"--run-for-ms"}, std::string_view{"250"},
      std::string_view{"--heartbeat-ms"}, std::string_view{"50"},
      std::string_view{"--instance-id"}, std::string_view{"ctest"},
      std::string_view{"--quiet"}};
  assert(vividcam::parse_engine_options(configured_arguments, options, error));
  assert(options.run_for == 250ms);
  assert(options.heartbeat_interval == 50ms);
  assert(options.instance_id == "ctest");
  assert(options.quiet);

  const std::array help_arguments = {std::string_view{"--help"}};
  assert(vividcam::parse_engine_options(help_arguments, options, error));
  assert(options.show_help);

  const std::array help_with_quiet = {
      std::string_view{"--help"}, std::string_view{"--quiet"}};
  assert(!vividcam::parse_engine_options(help_with_quiet, options, error));
  const std::array zero_duration = {
      std::string_view{"--run-for-ms"}, std::string_view{"0"}};
  assert(!vividcam::parse_engine_options(zero_duration, options, error));
  const std::array empty_duration = {
      std::string_view{"--run-for-ms"}, std::string_view{}};
  assert(!vividcam::parse_engine_options(empty_duration, options, error));
  const std::array malformed_duration = {
      std::string_view{"--heartbeat-ms"}, std::string_view{"10ms"}};
  assert(!vividcam::parse_engine_options(malformed_duration, options, error));
  const std::array missing_value = {std::string_view{"--instance-id"}};
  assert(!vividcam::parse_engine_options(missing_value, options, error));
  const std::array invalid_id = {
      std::string_view{"--instance-id"}, std::string_view{"bad id"}};
  assert(!vividcam::parse_engine_options(invalid_id, options, error));
  const std::array duplicate_option = {
      std::string_view{"--quiet"}, std::string_view{"--quiet"}};
  assert(!vividcam::parse_engine_options(duplicate_option, options, error));
  const std::array unknown_option = {std::string_view{"--unknown"}};
  assert(!vividcam::parse_engine_options(unknown_option, options, error));

  std::cout << "VIVIDCAM engine host tests passed\n";
  return 0;
}
