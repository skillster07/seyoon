#include "vividcam/engine_host.hpp"
#include "vividcam/engine_options.hpp"

#ifdef _WIN32
#include "vividcam/control_channel_transport.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
std::atomic<bool> stop_signal_received{false};

BOOL WINAPI engine_console_handler(DWORD control_type) {
  switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      stop_signal_received.store(true, std::memory_order_relaxed);
      return TRUE;
    default:
      return FALSE;
  }
}

bool install_stop_signal_handler() {
  stop_signal_received.store(false, std::memory_order_relaxed);
  return SetConsoleCtrlHandler(engine_console_handler, TRUE) != FALSE;
}

void uninstall_stop_signal_handler() {
  SetConsoleCtrlHandler(engine_console_handler, FALSE);
}

bool should_stop_for_signal() {
  return stop_signal_received.load(std::memory_order_relaxed);
}

std::uint64_t current_process_id() {
  return static_cast<std::uint64_t>(GetCurrentProcessId());
}

void print_control_status(
    std::string_view event,
    const vividcam::ControlChannelTransportSnapshot& status) {
  std::cout << "[engine-control] schema=" << status.schema_version
            << " event=" << event
            << " running=" << (status.running ? "true" : "false")
            << " connected=" << (status.connected ? "true" : "false")
            << " connection_attempts=" << status.connection_attempts
            << " successful_handshakes=" << status.successful_handshakes
            << " heartbeats_sent=" << status.heartbeats_sent
            << " heartbeat_acks=" << status.heartbeat_acks
            << " protocol_errors=" << status.protocol_errors
            << " rejected_peers=" << status.rejected_peers << std::endl;
}

void print_control_unavailable(std::string_view reason) {
  std::cout << "[engine-control] schema="
            << vividcam::kControlChannelTransportSchemaVersion
            << " event=unavailable reason=" << reason << std::endl;
}
#else
volatile std::sig_atomic_t stop_signal_received = 0;
using SignalHandler = void (*)(int);
SignalHandler previous_sigint = SIG_DFL;
SignalHandler previous_sigterm = SIG_DFL;

extern "C" void engine_signal_handler(int) {
  stop_signal_received = 1;
}

bool install_stop_signal_handler() {
  stop_signal_received = 0;
  previous_sigint = std::signal(SIGINT, engine_signal_handler);
  if (previous_sigint == SIG_ERR) return false;
  previous_sigterm = std::signal(SIGTERM, engine_signal_handler);
  if (previous_sigterm == SIG_ERR) {
    std::signal(SIGINT, previous_sigint);
    return false;
  }
  return true;
}

void uninstall_stop_signal_handler() {
  std::signal(SIGINT, previous_sigint);
  std::signal(SIGTERM, previous_sigterm);
}

bool should_stop_for_signal() {
  return stop_signal_received != 0;
}

std::uint64_t current_process_id() {
  return static_cast<std::uint64_t>(getpid());
}
#endif

std::string generated_instance_id() {
  const auto timestamp = vividcam::EngineHost::Clock::now().time_since_epoch().count();
  std::ostringstream output;
  output << "engine-" << current_process_id() << '-' << std::hex << timestamp;
  return output.str();
}

void print_status(std::string_view event, const vividcam::EngineHostSnapshot& status) {
  std::cout << "[engine] schema=" << status.schema_version
            << " event=" << event
            << " instance=" << status.instance_id
            << " state=" << vividcam::engine_lifecycle_state_name(status.state)
            << " heartbeat_seq=" << status.heartbeat_sequence
            << " missed_heartbeat_intervals=" << status.missed_heartbeat_intervals
            << " uptime_ms=" << status.uptime_ms
            << " frame_transport="
            << (status.frame_transport_ready ? "ready" : "unavailable")
            << " stop_reason=" << vividcam::engine_stop_reason_name(status.stop_reason)
            << std::endl;
}

} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);

  vividcam::EngineOptions options;
  std::string error;
  if (!vividcam::parse_engine_options(arguments, options, error)) {
    std::cerr << "vividcam_engine: " << error << "\n\n"
              << vividcam::engine_usage();
    return 2;
  }
  if (options.show_help) {
    std::cout << vividcam::engine_usage();
    return 0;
  }
  if (options.show_version) {
    std::cout << "vividcam_engine " << vividcam::kEngineVersion
              << " telemetry-schema=" << vividcam::kEngineTelemetrySchemaVersion << '\n';
    return 0;
  }

  const std::string instance_id =
      options.instance_id.value_or(generated_instance_id());
  vividcam::EngineHost host(instance_id, options.heartbeat_interval);
  const auto starting_at = vividcam::EngineHost::Clock::now();
  if (!host.begin_start(starting_at, error)) {
    std::cerr << "vividcam_engine: " << error << '\n';
    return 3;
  }
  print_status("lifecycle", host.snapshot(starting_at));

  if (!install_stop_signal_handler()) {
    const auto failed_at = vividcam::EngineHost::Clock::now();
    std::string transition_error;
    host.mark_failed("could not install the console stop handler", failed_at,
                     transition_error);
    print_status("lifecycle", host.snapshot(failed_at));
    std::cerr << "vividcam_engine: could not install the console stop handler\n";
    return 3;
  }

  const auto running_at = vividcam::EngineHost::Clock::now();
  if (!host.mark_running(running_at, error)) {
    uninstall_stop_signal_handler();
    std::cerr << "vividcam_engine: " << error << '\n';
    return 3;
  }
  print_status("lifecycle", host.snapshot(running_at));

#ifdef _WIN32
  vividcam::ProducerControlServer control_server;
  bool control_server_started = false;
  std::wstring control_route;
  std::string control_error;
  if (!vividcam::find_registered_vividcam_control_route(control_route,
                                                        control_error)) {
    print_control_unavailable("route-discovery-failed");
  } else if (!control_server.start(std::move(control_route), instance_id,
                                   control_error)) {
    print_control_unavailable("server-start-failed");
  } else {
    control_server_started = true;
    print_control_status("started", control_server.snapshot());
  }
#endif

  auto run_deadline = vividcam::EngineHost::TimePoint::max();
  if (options.run_for) run_deadline = running_at + *options.run_for;
  constexpr auto kSignalPollInterval = std::chrono::milliseconds{25};
  bool runtime_failed = false;

  while (true) {
    const auto now = vividcam::EngineHost::Clock::now();
    vividcam::EngineStopReason stop_reason = vividcam::EngineStopReason::None;
    if (should_stop_for_signal()) {
      stop_reason = vividcam::EngineStopReason::ConsoleSignal;
    } else if (now >= run_deadline) {
      stop_reason = vividcam::EngineStopReason::RunForElapsed;
    }

    if (stop_reason != vividcam::EngineStopReason::None) {
      if (!host.request_stop(stop_reason, now, error)) {
        runtime_failed = true;
      }
      break;
    }

    const auto tick_result = host.tick(now, error);
    if (tick_result == vividcam::EngineTickResult::Rejected) {
      runtime_failed = true;
      break;
    }
    if (tick_result == vividcam::EngineTickResult::Heartbeat && !options.quiet) {
      print_status("heartbeat", host.snapshot(now));
    }

    auto wake_at = now + kSignalPollInterval;
    if (const auto heartbeat_at = host.next_heartbeat_deadline()) {
      wake_at = std::min(wake_at, *heartbeat_at);
    }
    wake_at = std::min(wake_at, run_deadline);
    if (wake_at > now) std::this_thread::sleep_until(wake_at);
  }

#ifdef _WIN32
  if (control_server_started) {
    control_server.stop();
    print_control_status("stopped", control_server.snapshot());
  }
#endif

  if (runtime_failed) {
    const auto failed_at = vividcam::EngineHost::Clock::now();
    std::string transition_error;
    host.mark_failed(error.empty() ? "engine runtime failed" : error, failed_at,
                     transition_error);
    print_status("lifecycle", host.snapshot(failed_at));
    uninstall_stop_signal_handler();
    std::cerr << "vividcam_engine: "
              << (error.empty() ? "engine runtime failed" : error) << '\n';
    return 4;
  }

  auto stopped_at = vividcam::EngineHost::Clock::now();
  print_status("lifecycle", host.snapshot(stopped_at));
  if (!host.mark_stopped(stopped_at, error)) {
    uninstall_stop_signal_handler();
    std::cerr << "vividcam_engine: " << error << '\n';
    return 4;
  }
  stopped_at = vividcam::EngineHost::Clock::now();
  print_status("lifecycle", host.snapshot(stopped_at));
  uninstall_stop_signal_handler();
  return 0;
}
