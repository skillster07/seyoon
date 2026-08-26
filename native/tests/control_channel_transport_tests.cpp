#include "vividcam/control_channel_transport.hpp"

#include <algorithm>
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
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Aclapi.h>
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
struct KernelObjectAccessSummary {
  ACCESS_MASK allowed{0};
  ACCESS_MASK denied{0};
  std::uint32_t allowed_ace_count{0};
};

std::vector<std::byte> frame_server_service_sid() {
  constexpr wchar_t account[] = L"NT SERVICE\\FrameServer";
  DWORD sid_bytes = 0;
  DWORD domain_characters = 0;
  SID_NAME_USE sid_type = SidTypeUnknown;
  assert(!LookupAccountNameW(nullptr, account, nullptr, &sid_bytes, nullptr,
                             &domain_characters, &sid_type));
  assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
  assert(sid_bytes != 0);
  std::vector<std::byte> sid(sid_bytes, std::byte{0});
  std::vector<wchar_t> domain(std::max<DWORD>(domain_characters, 1U), L'\0');
  DWORD sid_capacity = sid_bytes;
  DWORD domain_capacity = static_cast<DWORD>(domain.size());
  assert(LookupAccountNameW(nullptr, account, sid.data(), &sid_capacity,
                            domain.data(), &domain_capacity, &sid_type));
  assert(IsValidSid(sid.data()));
  sid.resize(sid_capacity);
  return sid;
}

KernelObjectAccessSummary inspect_sid_access(HANDLE object, PSID target_sid) {
  assert(target_sid != nullptr);
  assert(IsValidSid(target_sid));

  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  assert(GetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                         nullptr, nullptr, &dacl, nullptr,
                         &descriptor) == ERROR_SUCCESS);
  assert(descriptor != nullptr);
  assert(dacl != nullptr);
  assert(IsValidAcl(dacl));

  KernelObjectAccessSummary summary;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    assert(GetAce(dacl, index, &raw_ace));
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
      const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
      PSID sid = const_cast<DWORD*>(&ace->SidStart);
      if (IsValidSid(sid) && EqualSid(sid, target_sid)) {
        summary.allowed |= ace->Mask;
        ++summary.allowed_ace_count;
      }
    } else if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
      const auto* ace = static_cast<const ACCESS_DENIED_ACE*>(raw_ace);
      PSID sid = const_cast<DWORD*>(&ace->SidStart);
      if (IsValidSid(sid) && EqualSid(sid, target_sid)) {
        summary.denied |= ace->Mask;
      }
    }
  }
  LocalFree(descriptor);
  return summary;
}

struct EngineQueryGrantSnapshot {
  KernelObjectAccessSummary process;
  KernelObjectAccessSummary token;
};

EngineQueryGrantSnapshot inspect_engine_query_grants() {
  std::vector<std::byte> service_sid = frame_server_service_sid();
  const HANDLE process = OpenProcess(READ_CONTROL, FALSE, GetCurrentProcessId());
  assert(process != nullptr);
  HANDLE token = nullptr;
  assert(OpenProcessToken(GetCurrentProcess(), READ_CONTROL, &token));
  const EngineQueryGrantSnapshot result{inspect_sid_access(process,
                                                            service_sid.data()),
                                        inspect_sid_access(token,
                                                           service_sid.data())};
  CloseHandle(token);
  CloseHandle(process);
  return result;
}

bool current_user_is_denied_by_production_pipe(
    std::wstring_view pipe_name, std::chrono::milliseconds timeout) {
  const std::wstring owned_pipe_name(pipe_name);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const DWORD flags = SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;
    const HANDLE pipe = CreateFileW(
        owned_pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, flags, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe);
      return false;
    }
    const DWORD status = GetLastError();
    if (status == ERROR_ACCESS_DENIED) return true;
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PIPE_BUSY) {
      return false;
    }
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

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
  assert(!vividcam::kVividCamPrimaryControlRoute.empty());
  std::wstring primary_pipe_name;
  assert(vividcam::make_vividcam_control_pipe_name(
      vividcam::kVividCamPrimaryControlRoute, primary_pipe_name, error));
  assert(primary_pipe_name ==
         std::wstring(prefix) +
             L"0929f54cd590114b325ccb3feaac79b18330489553d477a9b2d88109e7339f4f");
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

  EngineQueryGrantSnapshot first_query_grants;

  {
    // The canonical endpoint is service-only. Even the interactive account
    // that owns the engine process must not be able to occupy its pipe.
    ProducerControlServer production_server;
    assert(production_server.start(
        std::wstring(vividcam::kVividCamPrimaryControlRoute),
        "production-policy-test", error));
    assert(wait_until(
        [&] { return production_server.snapshot().connection_attempts >= 1; },
        1s));
    assert(current_user_is_denied_by_production_pipe(primary_pipe_name, 1s));
    production_server.stop();
    assert(!production_server.snapshot().running);
  }

  {
    ProducerControlServer server;
    SourceControlClient client;
    assert(server.start(route, "loopback-engine", error));
    first_query_grants = inspect_engine_query_grants();
    assert(first_query_grants.process.allowed ==
           PROCESS_QUERY_LIMITED_INFORMATION);
    assert(first_query_grants.process.denied == 0);
    assert(first_query_grants.process.allowed_ace_count == 1);
    assert((first_query_grants.process.allowed &
            (PROCESS_TERMINATE | PROCESS_VM_OPERATION | PROCESS_VM_READ |
             PROCESS_VM_WRITE)) == 0);
    assert(first_query_grants.token.allowed == TOKEN_QUERY);
    assert(first_query_grants.token.denied == 0);
    assert(first_query_grants.token.allowed_ace_count == 1);
    assert((first_query_grants.token.allowed &
            (TOKEN_DUPLICATE | TOKEN_IMPERSONATE)) == 0);
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

  const auto repeated_query_grants = inspect_engine_query_grants();
  assert(repeated_query_grants.process.allowed ==
         first_query_grants.process.allowed);
  assert(repeated_query_grants.process.denied ==
         first_query_grants.process.denied);
  assert(repeated_query_grants.process.allowed_ace_count ==
         first_query_grants.process.allowed_ace_count);
  assert(repeated_query_grants.token.allowed ==
         first_query_grants.token.allowed);
  assert(repeated_query_grants.token.denied ==
         first_query_grants.token.denied);
  assert(repeated_query_grants.token.allowed_ace_count ==
         first_query_grants.token.allowed_ace_count);

  return 0;
#endif
}
