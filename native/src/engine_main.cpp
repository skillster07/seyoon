#include "vividcam/engine_host.hpp"
#include "vividcam/engine_options.hpp"

#ifdef _WIN32
#include "vividcam/control_channel_transport.hpp"
#include "vividcam/engine_frame_publisher.hpp"
#include "vividcam/engine_frame_worker.hpp"
#include "vividcam/latency_tracker.hpp"

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
#include <optional>
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

std::string telemetry_token(std::string_view value) {
  if (value.empty()) return "none";
  std::string result;
  result.reserve(std::min<std::size_t>(value.size(), 160U));
  for (const char character : value) {
    if (result.size() == 160U) break;
    const bool alpha_numeric =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
    result.push_back(alpha_numeric || character == '-' || character == '.'
                         ? character
                         : '_');
  }
  return result.empty() ? "none" : result;
}

void print_frame_status(
    std::string_view event,
    const vividcam::EngineFrameWorkerSnapshot& worker,
    const vividcam::EngineFramePublisherSnapshot& publisher,
    const vividcam::CpuFrameMailboxSnapshot& mailbox,
    const vividcam::LatencySnapshot& publish_latency) {
  const auto& pipeline = worker.pipeline;
  const char* state = !worker.enabled_requested ? "inactive"
                      : worker.pipeline_ready   ? "ready"
                                                : "degraded";
  std::cout << "[engine-frame] schema=" << worker.schema_version
            << " event=" << event << " state=" << state
            << " mailbox=" << (mailbox.open ? "ready" : "unavailable")
            << " due=" << publisher.due_frames
            << " published=" << publisher.published_frames
            << " repeated=" << publisher.repeated_frames
            << " deadline_drops=" << publisher.deadline_drops
            << " no_input=" << publisher.no_input_frames
            << " worker_no_frame_polls=" << worker.no_frame_polls
            << " transport_unavailable="
            << publisher.transport_unavailable_frames
            << " publish_failures=" << publisher.publish_failures
            << " pipeline_attempts=" << worker.pipeline_start_attempts
            << " pipeline_restarts=" << worker.pipeline_restarts
            << " worker_produced=" << worker.produced_frames
            << " worker_consumed=" << worker.consumed_frames
            << " worker_overwritten=" << worker.overwritten_frames
            << " worker_deadline_drops=" << worker.deadline_drops
            << " capture_new=" << pipeline.new_capture_frames
            << " capture_repeated=" << pipeline.repeated_capture_frames
            << " render_failures=" << pipeline.render_failures
            << " conversion_failures=" << pipeline.conversion_failures
            << " readback_failures=" << pipeline.readback_failures
            << " readback_p95_ms=" << pipeline.readback_latency.p95_ms
            << " publish_p95_ms=" << publish_latency.p95_ms
            << " mailbox_overwritten=" << mailbox.overwritten_frames
            << " last_error=" << telemetry_token(worker.last_error)
            << std::endl;
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
  vividcam::EngineFramePublisher frame_publisher;
  vividcam::EngineFrameWorkerOptions frame_worker_options;
  frame_worker_options.start_enabled = false;
  vividcam::EngineFrameWorker frame_worker(frame_worker_options);
  vividcam::CpuNv12Frame publisher_frame;
  vividcam::LatencyTracker publish_latency(600);
  bool control_server_started = false;
  bool frame_worker_started = false;
  bool frame_worker_enabled = false;
  std::wstring active_mailbox_name;
  std::optional<vividcam::EngineFrameSourceIdentity> cached_source_identity;
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
    if (!frame_worker.start(control_error)) {
      std::cerr << "vividcam_engine: " << control_error << '\n';
    } else {
      frame_worker_started = true;
    }
  }
#endif

  auto run_deadline = vividcam::EngineHost::TimePoint::max();
  if (options.run_for) run_deadline = running_at + *options.run_for;
  constexpr auto kSignalPollInterval = std::chrono::milliseconds{25};
  bool runtime_failed = false;
#ifdef _WIN32
  if (control_server_started && !frame_worker_started) {
    error = control_error.empty() ? "could not start the engine frame worker"
                                  : control_error;
    runtime_failed = true;
  }
#endif

  while (true) {
    if (runtime_failed) break;
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
    const bool emit_heartbeat =
        tick_result == vividcam::EngineTickResult::Heartbeat && !options.quiet;

#ifdef _WIN32
    if (frame_worker_started && !frame_worker.running()) {
      error = "engine frame worker stopped unexpectedly";
      runtime_failed = true;
      break;
    }
    vividcam::CpuFrameMailboxSnapshot mailbox_status;
    const auto frame_now = vividcam::EngineHost::Clock::now();
    std::wstring observed_mailbox_name =
        control_server_started ? control_server.frame_mailbox_name()
                               : std::wstring{};
    const bool should_enable_frame_worker =
        frame_worker_started && !observed_mailbox_name.empty();
    if (frame_worker_started &&
        should_enable_frame_worker != frame_worker_enabled) {
      frame_worker.set_enabled(should_enable_frame_worker);
      frame_worker_enabled = should_enable_frame_worker;
    }
    if (observed_mailbox_name != active_mailbox_name) {
      frame_publisher.set_transport_ready(false, frame_now);
      active_mailbox_name = std::move(observed_mailbox_name);
      cached_source_identity.reset();
      publisher_frame.sequence = 0;
      publisher_frame.timestamp_100ns = 0;
      publisher_frame.bytes.clear();
      if (!active_mailbox_name.empty()) {
        // Mailbox readiness is independent from camera/GPU readiness. Tickets
        // continue to expose NoInput while the asynchronous worker recovers.
        frame_publisher.set_transport_ready(true, frame_now);
      }
      mailbox_status = control_server_started
                           ? control_server.frame_mailbox_snapshot()
                           : vividcam::CpuFrameMailboxSnapshot{};
      print_frame_status(active_mailbox_name.empty() ? "disconnected"
                                                      : "connected",
                         frame_worker.snapshot(), frame_publisher.snapshot(),
                         mailbox_status, publish_latency.snapshot());
    }

    if (!active_mailbox_name.empty()) {
      if (const auto ticket = frame_publisher.begin_frame(frame_now)) {
        vividcam::EngineFramePublishOutcome outcome =
            vividcam::EngineFramePublishOutcome::NoInput;
        std::uint64_t pipeline_generation = 0;
        if (frame_worker_started && frame_worker.take_latest_cpu_frame(
                                        publisher_frame,
                                        pipeline_generation)) {
          cached_source_identity = vividcam::EngineFrameSourceIdentity{
              pipeline_generation, publisher_frame.sequence};
        }

        if (cached_source_identity && publisher_frame.valid()) {
          publisher_frame.sequence = ticket->sequence;
          publisher_frame.timestamp_100ns = ticket->timestamp_100ns;
          std::string frame_error;
          const auto publish_started = vividcam::EngineHost::Clock::now();
          const auto publish_result =
              control_server.publish_cpu_frame_for_mailbox(
                  publisher_frame, active_mailbox_name, frame_error);
          publish_latency.record(std::chrono::duration<double, std::milli>(
                                     vividcam::EngineHost::Clock::now() -
                                     publish_started)
                                     .count());
          switch (publish_result) {
            case vividcam::CpuFramePublishResult::Published:
              outcome = vividcam::EngineFramePublishOutcome::Published;
              break;
            case vividcam::CpuFramePublishResult::TransportUnavailable:
            case vividcam::CpuFramePublishResult::MailboxChanged:
              outcome =
                  vividcam::EngineFramePublishOutcome::TransportUnavailable;
              break;
            case vividcam::CpuFramePublishResult::Failed:
              outcome = vividcam::EngineFramePublishOutcome::PublishFailed;
              break;
          }
        }

        const auto completed_at = vividcam::EngineHost::Clock::now();
        if (!frame_publisher.complete_frame(
                *ticket, completed_at, outcome, cached_source_identity)) {
          error = "engine frame publisher rejected its active frame ticket";
          runtime_failed = true;
          break;
        }
        if (outcome ==
            vividcam::EngineFramePublishOutcome::TransportUnavailable) {
          frame_publisher.set_transport_ready(false, completed_at);
          active_mailbox_name.clear();
          cached_source_identity.reset();
          publisher_frame.bytes.clear();
        }
      }
    }
#endif

    if (emit_heartbeat) {
      auto status = host.snapshot(vividcam::EngineHost::Clock::now());
#ifdef _WIN32
      mailbox_status = control_server_started
                           ? control_server.frame_mailbox_snapshot()
                           : vividcam::CpuFrameMailboxSnapshot{};
      status.frame_transport_ready = mailbox_status.open;
#endif
      print_status("heartbeat", status);
#ifdef _WIN32
      print_frame_status("heartbeat", frame_worker.snapshot(),
                         frame_publisher.snapshot(), mailbox_status,
                         publish_latency.snapshot());
#endif
    }

    const auto sleep_now = vividcam::EngineHost::Clock::now();
    auto wake_at = sleep_now + kSignalPollInterval;
    if (const auto heartbeat_at = host.next_heartbeat_deadline()) {
      wake_at = std::min(wake_at, *heartbeat_at);
    }
#ifdef _WIN32
    if (const auto frame_at = frame_publisher.next_deadline()) {
      wake_at = std::min(wake_at, *frame_at);
    }
#endif
    wake_at = std::min(wake_at, run_deadline);
    if (wake_at > sleep_now) std::this_thread::sleep_until(wake_at);
  }

#ifdef _WIN32
  if (control_server_started) {
    frame_publisher.set_transport_ready(
        false, vividcam::EngineHost::Clock::now());
    if (frame_worker_started) {
      frame_worker.set_enabled(false);
    }
    // Close the source session first so a slow camera driver cannot delay the
    // Frame Server's disconnect observation during worker shutdown.
    control_server.stop();
    if (frame_worker_started) frame_worker.stop();
    print_frame_status("stopped", frame_worker.snapshot(),
                       frame_publisher.snapshot(),
                       control_server.frame_mailbox_snapshot(),
                       publish_latency.snapshot());
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
