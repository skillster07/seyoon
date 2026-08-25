#include "vividcam/control_channel_transport.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

#ifdef _WIN32
bool send_wrong_magic_header(std::wstring_view pipe_name,
                             std::chrono::milliseconds timeout) {
  const std::wstring owned_pipe_name(pipe_name);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const DWORD flags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
    const HANDLE pipe = CreateFileW(
        owned_pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, flags, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      std::array<std::byte, 64> wrong_magic{};
      DWORD written = 0;
      const bool sent =
          WriteFile(pipe, wrong_magic.data(),
                    static_cast<DWORD>(wrong_magic.size()), &written,
                    nullptr) != FALSE &&
          written == wrong_magic.size();
      CloseHandle(pipe);
      return sent;
    }
    const DWORD status = GetLastError();
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PIPE_BUSY) {
      return false;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}
#endif

} // namespace

int main() {
  using vividcam::ProducerControlServer;
  using vividcam::SourceControlClient;

#ifndef _WIN32
  std::string error;
  std::wstring pipe_name;
  assert(!vividcam::make_vividcam_control_pipe_name(L"route", pipe_name,
                                                    error));
  assert(!error.empty());
  return 0;
#else
  const std::wstring route =
      L"vividcam-control-transport-test-" +
      std::to_wstring(static_cast<std::uint64_t>(GetCurrentProcessId())) + L"-" +
      std::to_wstring(static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string error;
  std::wstring pipe_name;
  assert(vividcam::make_vividcam_control_pipe_name(route, pipe_name, error));
  assert(error.empty());
  constexpr std::wstring_view prefix =
      L"\\\\.\\pipe\\VIVIDCAM.Control.v1.";
  assert(pipe_name.starts_with(prefix));
  assert(pipe_name.size() == prefix.size() + 64);
  assert(pipe_name.find(route) == std::wstring::npos);
  for (wchar_t character : std::wstring_view(pipe_name).substr(prefix.size())) {
    assert((character >= L'0' && character <= L'9') ||
           (character >= L'a' && character <= L'f'));
  }
  std::wstring repeated_name;
  assert(vividcam::make_vividcam_control_pipe_name(route, repeated_name, error));
  assert(repeated_name == pipe_name);
  std::wstring golden_name;
  assert(vividcam::make_vividcam_control_pipe_name(
      L"vividcam-control-test-route", golden_name, error));
  assert(golden_name ==
         std::wstring(prefix) +
             L"f9ac56bda3d725785924961067b26402b1f78f4adeb0ded1f5b53c3f8cbc870d");
  std::wstring different_name;
  assert(vividcam::make_vividcam_control_pipe_name(route + L"-other",
                                                   different_name, error));
  assert(different_name != pipe_name);
  std::wstring invalid_name;
  assert(!vividcam::make_vividcam_control_pipe_name({}, invalid_name, error));
  assert(!error.empty());

  {
    ProducerControlServer server;
    SourceControlClient client;
    assert(server.start(route, "loopback-engine", error));
    assert(send_wrong_magic_header(pipe_name, 1s));
    assert(wait_until(
        [&] { return server.snapshot().protocol_errors >= 1; }, 1s));
    assert(client.start(route, error));
    const bool loopback_ready = wait_until(
        [&] {
          const auto server_status = server.snapshot();
          const auto client_status = client.snapshot();
          return server_status.connected && client_status.connected &&
                 server_status.successful_handshakes >= 1 &&
                 client_status.successful_handshakes >= 1 &&
                 server_status.heartbeat_acks >= 2;
        },
        4s);
    if (!loopback_ready) {
      const auto server_debug = server.snapshot();
      const auto client_debug = client.snapshot();
      std::cerr << "server attempts=" << server_debug.connection_attempts
                << " handshakes=" << server_debug.successful_handshakes
                << " sent=" << server_debug.heartbeats_sent
                << " acks=" << server_debug.heartbeat_acks
                << " rejected=" << server_debug.rejected_peers
                << " protocol=" << server_debug.protocol_errors
                << " error=" << server_debug.last_error << '\n'
                << "client attempts=" << client_debug.connection_attempts
                << " handshakes=" << client_debug.successful_handshakes
                << " acks=" << client_debug.heartbeat_acks
                << " protocol=" << client_debug.protocol_errors
                << " error=" << client_debug.last_error << '\n';
    }
    assert(loopback_ready);

    const auto server_status = server.snapshot();
    const auto client_status = client.snapshot();
    assert(server_status.schema_version == 1);
    assert(client_status.schema_version == 1);
    assert(server_status.peer_process_id == GetCurrentProcessId());
    assert(client_status.peer_process_id == GetCurrentProcessId());
    assert(server_status.heartbeats_sent >= 2);
    assert(server_status.heartbeat_acks >= 2);
    assert(client_status.heartbeat_acks >= 2);
    assert(server_status.protocol_errors == 1);
    assert(client_status.protocol_errors == 0);
    assert(server_status.rejected_peers == 0);

    const auto stop_started = std::chrono::steady_clock::now();
    client.stop();
    server.stop();
    assert(std::chrono::steady_clock::now() - stop_started < 2s);
    assert(!client.snapshot().running);
    assert(!server.snapshot().running);
    client.stop();
    server.stop();
  }

  {
    const std::wstring reconnect_route = route + L"-reconnect";
    SourceControlClient client;
    ProducerControlServer server;
    assert(client.start(reconnect_route, error));
    assert(wait_until(
        [&] { return client.snapshot().connection_attempts >= 2; }, 1s));
    assert(!client.snapshot().connected);

    assert(server.start(reconnect_route, "reconnect-engine", error));
    assert(wait_until(
        [&] {
          return client.snapshot().successful_handshakes >= 1 &&
                 server.snapshot().successful_handshakes >= 1;
        },
        3s));
    const auto attempts_before_stop = client.snapshot().connection_attempts;

    server.stop();
    assert(wait_until(
        [&] {
          const auto status = client.snapshot();
          return !status.connected &&
                 status.connection_attempts > attempts_before_stop;
        },
        2s));

    assert(server.start(reconnect_route, "restarted-engine", error));
    assert(wait_until(
        [&] {
          return client.snapshot().successful_handshakes >= 2 &&
                 server.snapshot().successful_handshakes >= 1 &&
                 server.snapshot().heartbeat_acks >= 1;
        },
        3s));
    assert(client.snapshot().protocol_errors == 0);
    assert(server.snapshot().protocol_errors == 0);
    assert(server.snapshot().rejected_peers == 0);

    const auto stop_started = std::chrono::steady_clock::now();
    client.stop();
    server.stop();
    assert(std::chrono::steady_clock::now() - stop_started < 2s);
  }

  {
    // Concurrent lifecycle churn exercises start-vs-stop serialization and
    // the published-handle lifetime invariant while ConnectNamedPipe/CreateFile
    // operations are repeatedly cancelled.
    const std::wstring lifecycle_route = route + L"-lifecycle";
    ProducerControlServer server;
    SourceControlClient client;
    std::atomic<bool> unexpected_failure{false};
    auto churn_server = [&] {
      for (int iteration = 0; iteration < 12; ++iteration) {
        std::string local_error;
        if (!server.start(lifecycle_route, "lifecycle-engine", local_error) &&
            local_error != "Producer control server is already running") {
          unexpected_failure.store(true, std::memory_order_relaxed);
        }
        server.stop();
      }
    };
    auto churn_client = [&] {
      for (int iteration = 0; iteration < 12; ++iteration) {
        std::string local_error;
        if (!client.start(lifecycle_route, local_error) &&
            local_error != "Source control client is already running") {
          unexpected_failure.store(true, std::memory_order_relaxed);
        }
        client.stop();
      }
    };
    std::thread server_a(churn_server);
    std::thread server_b(churn_server);
    std::thread client_a(churn_client);
    std::thread client_b(churn_client);
    server_a.join();
    server_b.join();
    client_a.join();
    client_b.join();
    client.stop();
    server.stop();
    assert(!unexpected_failure.load(std::memory_order_relaxed));
    assert(!client.snapshot().running);
    assert(!server.snapshot().running);
  }

  return 0;
#endif
}
