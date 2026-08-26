#include "vividcam/control_channel_transport.hpp"

#include "vividcam/control_channel_state.hpp"
#include "vividcam/cpu_frame_transport.hpp"
#include "vividcam/producer_identity.hpp"
#include "vividcam/producer_ipc_protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <bcrypt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vividcam {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using producer_ipc::ConnectionId;
using producer_ipc::MessageHeader;
using producer_ipc::MessageType;

constexpr std::chrono::milliseconds kHandshakeTimeout{2000};
constexpr std::chrono::milliseconds kHeartbeatInterval{500};
constexpr std::chrono::milliseconds kHeartbeatAckTimeout{1500};
constexpr std::chrono::milliseconds kServerRetryDelay{100};
constexpr DWORD kPipeBufferBytes =
    producer_ipc::kHeaderBytes + producer_ipc::kMaximumPayloadBytes;
constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\VIVIDCAM.Control.v1.";
constexpr wchar_t kFrameServerServiceName[] = L"FrameServer";
constexpr wchar_t kFrameServerAccountName[] = L"NT SERVICE\\FrameServer";

bool uses_production_peer_policy(std::wstring_view route) noexcept {
  return route == kVividCamPrimaryControlRoute;
}

class UniqueHandle {
 public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueHandle() { reset(); }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) reset(std::exchange(other.handle_, nullptr));
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }
  HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
  void reset(HANDLE replacement = nullptr) noexcept {
    if (valid()) CloseHandle(handle_);
    handle_ = replacement;
  }

 private:
  HANDLE handle_{nullptr};
};

class UniqueServiceHandle {
 public:
  UniqueServiceHandle() noexcept = default;
  explicit UniqueServiceHandle(SC_HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueServiceHandle() {
    if (handle_ != nullptr) CloseServiceHandle(handle_);
  }
  UniqueServiceHandle(const UniqueServiceHandle&) = delete;
  UniqueServiceHandle& operator=(const UniqueServiceHandle&) = delete;
  [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

 private:
  SC_HANDLE handle_{nullptr};
};

class LocalAllocation {
 public:
  LocalAllocation() noexcept = default;
  explicit LocalAllocation(HLOCAL value) noexcept : value_(value) {}
  ~LocalAllocation() {
    if (value_ != nullptr) LocalFree(value_);
  }
  LocalAllocation(const LocalAllocation&) = delete;
  LocalAllocation& operator=(const LocalAllocation&) = delete;
  [[nodiscard]] HLOCAL get() const noexcept { return value_; }

 private:
  HLOCAL value_{nullptr};
};

std::string windows_error(const char* operation, DWORD status) {
  std::ostringstream message;
  message << operation << " failed (Win32=" << status << ')';
  return message.str();
}

std::string hresult_error(const char* operation, HRESULT status) {
  std::ostringstream message;
  message << operation << " failed (HRESULT=0x" << std::hex << std::uppercase
          << std::setw(8) << std::setfill('0')
          << static_cast<std::uint32_t>(status) << ')';
  return message.str();
}

std::string ntstatus_error(const char* operation, NTSTATUS status) {
  std::ostringstream message;
  message << operation << " failed (NTSTATUS=0x" << std::hex << std::uppercase
          << std::setw(8) << std::setfill('0')
          << static_cast<std::uint32_t>(status) << ')';
  return message.str();
}

DWORD remaining_timeout(Clock::time_point deadline) noexcept {
  const auto now = Clock::now();
  if (deadline <= now) return 0;
  const auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  const auto value = std::max<std::int64_t>(1, duration.count());
  return value >= static_cast<std::int64_t>(INFINITE - 1)
             ? INFINITE - 1
             : static_cast<DWORD>(value);
}

bool stop_requested(HANDLE stop_event) noexcept {
  return WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
}

enum class IoResult {
  Complete,
  Timeout,
  Stopped,
  Failed,
  ProtocolFailure,
  ReconnectScheduled,
};

IoResult finish_overlapped(HANDLE pipe, HANDLE stop_event,
                           OVERLAPPED& overlapped,
                           Clock::time_point deadline, DWORD& transferred,
                           std::string& error) {
  const std::array<HANDLE, 2> waits{stop_event, overlapped.hEvent};
  const DWORD result = WaitForMultipleObjects(
      static_cast<DWORD>(waits.size()), waits.data(), FALSE,
      remaining_timeout(deadline));
  if (result == WAIT_OBJECT_0 + 1) {
    if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
      error.clear();
      return IoResult::Complete;
    }
    const DWORD status = GetLastError();
    if (status == ERROR_OPERATION_ABORTED && stop_requested(stop_event)) {
      return IoResult::Stopped;
    }
    error = windows_error("GetOverlappedResult", status);
    return IoResult::Failed;
  }

  const bool stopped = result == WAIT_OBJECT_0;
  const bool timed_out = result == WAIT_TIMEOUT;
  (void)CancelIoEx(pipe, &overlapped);
  DWORD ignored = 0;
  if (!GetOverlappedResult(pipe, &overlapped, &ignored, TRUE)) {
    const DWORD status = GetLastError();
    if (status != ERROR_OPERATION_ABORTED) {
      error = windows_error("drain cancelled pipe I/O", status);
      return IoResult::Failed;
    }
  }
  if (stopped) {
    error.clear();
    return IoResult::Stopped;
  }
  if (timed_out) {
    error = "named-pipe operation timed out";
    return IoResult::Timeout;
  }
  error = windows_error("WaitForMultipleObjects", GetLastError());
  return IoResult::Failed;
}

IoResult read_exact(HANDLE pipe, HANDLE stop_event, std::span<std::byte> output,
                    Clock::time_point deadline, std::string& error) {
  std::size_t offset = 0;
  while (offset < output.size()) {
    if (Clock::now() >= deadline) {
      error = "named-pipe operation timed out";
      return IoResult::Timeout;
    }
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
      error = windows_error("CreateEvent(pipe read)", GetLastError());
      return IoResult::Failed;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const std::size_t remaining = output.size() - offset;
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
        remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD transferred = 0;
    const BOOL started = ReadFile(pipe, output.data() + offset, requested,
                                  nullptr, &overlapped);
    IoResult result = IoResult::Complete;
    if (!started) {
      const DWORD status = GetLastError();
      if (status != ERROR_IO_PENDING) {
        error = windows_error("ReadFile", status);
        return status == ERROR_OPERATION_ABORTED && stop_requested(stop_event)
                   ? IoResult::Stopped
                   : IoResult::Failed;
      }
      result = finish_overlapped(pipe, stop_event, overlapped, deadline,
                                 transferred, error);
    } else if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
      const DWORD status = GetLastError();
      error = windows_error("GetOverlappedResult(pipe read)", status);
      return IoResult::Failed;
    }
    if (result != IoResult::Complete) return result;
    if (transferred == 0) {
      error = "named-pipe peer closed the connection";
      return IoResult::Failed;
    }
    offset += transferred;
  }
  error.clear();
  return IoResult::Complete;
}

IoResult write_all(HANDLE pipe, HANDLE stop_event,
                   std::span<const std::byte> input,
                   Clock::time_point deadline, std::string& error) {
  std::size_t offset = 0;
  while (offset < input.size()) {
    if (Clock::now() >= deadline) {
      error = "named-pipe operation timed out";
      return IoResult::Timeout;
    }
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
      error = windows_error("CreateEvent(pipe write)", GetLastError());
      return IoResult::Failed;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const std::size_t remaining = input.size() - offset;
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
        remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
    DWORD transferred = 0;
    const BOOL started = WriteFile(pipe, input.data() + offset, requested,
                                   nullptr, &overlapped);
    IoResult result = IoResult::Complete;
    if (!started) {
      const DWORD status = GetLastError();
      if (status != ERROR_IO_PENDING) {
        error = windows_error("WriteFile", status);
        return status == ERROR_OPERATION_ABORTED && stop_requested(stop_event)
                   ? IoResult::Stopped
                   : IoResult::Failed;
      }
      result = finish_overlapped(pipe, stop_event, overlapped, deadline,
                                 transferred, error);
    } else if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
      const DWORD status = GetLastError();
      error = windows_error("GetOverlappedResult(pipe write)", status);
      return IoResult::Failed;
    }
    if (result != IoResult::Complete) return result;
    if (transferred == 0) {
      error = "named-pipe write completed without progress";
      return IoResult::Failed;
    }
    offset += transferred;
  }
  error.clear();
  return IoResult::Complete;
}

IoResult wait_for_header(HANDLE pipe, HANDLE stop_event,
                         Clock::time_point deadline, std::string& error) {
  while (!stop_requested(stop_event)) {
    if (Clock::now() >= deadline) {
      error = "named-pipe operation timed out";
      return IoResult::Timeout;
    }
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      error = windows_error("PeekNamedPipe", GetLastError());
      return IoResult::Failed;
    }
    if (available >= producer_ipc::kHeaderBytes) {
      error.clear();
      return IoResult::Complete;
    }
    const DWORD remaining = remaining_timeout(deadline);
    if (remaining == 0) {
      error = "named-pipe operation timed out";
      return IoResult::Timeout;
    }
    const DWORD poll = std::min<DWORD>(remaining, 25);
    const DWORD waited = WaitForSingleObject(stop_event, poll);
    if (waited == WAIT_OBJECT_0) {
      error.clear();
      return IoResult::Stopped;
    }
    if (waited != WAIT_TIMEOUT) {
      error = windows_error("WaitForSingleObject(pipe poll)", GetLastError());
      return IoResult::Failed;
    }
  }
  error.clear();
  return IoResult::Stopped;
}

IoResult read_message(HANDLE pipe, HANDLE stop_event, MessageHeader& header,
                      std::vector<std::byte>& payload,
                      Clock::time_point deadline, std::string& error) {
  payload.clear();
  const IoResult available =
      wait_for_header(pipe, stop_event, deadline, error);
  if (available != IoResult::Complete) return available;
  std::array<std::byte, producer_ipc::kHeaderBytes> encoded_header{};
  IoResult result = read_exact(pipe, stop_event, encoded_header, deadline, error);
  if (result != IoResult::Complete) return result;

  const producer_ipc::ProtocolError decoded =
      producer_ipc::decode_header(encoded_header, header);
  if (decoded != producer_ipc::ProtocolError::None) {
    error = std::string(producer_ipc::protocol_error_message(decoded));
    return IoResult::ProtocolFailure;
  }
  if (header.payload_bytes != 0) {
    try {
      payload.resize(header.payload_bytes);
    } catch (const std::bad_alloc&) {
      error = "Unable to allocate the bounded control message payload";
      return IoResult::Failed;
    }
    result = read_exact(pipe, stop_event, payload, deadline, error);
    if (result != IoResult::Complete) {
      payload.clear();
      return result;
    }
  }
  error.clear();
  return IoResult::Complete;
}

IoResult write_message(HANDLE pipe, HANDLE stop_event,
                       const MessageHeader& header,
                       std::span<const std::byte> payload,
                       Clock::time_point deadline, std::string& error) {
  std::vector<std::byte> encoded;
  const producer_ipc::ProtocolError status =
      producer_ipc::encode_message(header, payload, encoded);
  if (status != producer_ipc::ProtocolError::None) {
    error = std::string(producer_ipc::protocol_error_message(status));
    return IoResult::Failed;
  }
  return write_all(pipe, stop_event, encoded, deadline, error);
}

IoResult read_message(HANDLE pipe, HANDLE stop_event, MessageHeader& header,
                      Clock::time_point deadline, std::string& error) {
  std::vector<std::byte> payload;
  const IoResult result =
      read_message(pipe, stop_event, header, payload, deadline, error);
  if (result == IoResult::Complete && !payload.empty()) {
    error = "control message payload must be empty";
    return IoResult::ProtocolFailure;
  }
  return result;
}

IoResult write_message(HANDLE pipe, HANDLE stop_event,
                       const MessageHeader& header,
                       Clock::time_point deadline, std::string& error) {
  return write_message(pipe, stop_event, header, {}, deadline, error);
}

bool random_connection_id(ConnectionId& connection_id, std::string& error) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    const NTSTATUS status = BCryptGenRandom(
        nullptr, connection_id.data(),
        static_cast<ULONG>(connection_id.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
      error = ntstatus_error("BCryptGenRandom", status);
      return false;
    }
    if (std::any_of(connection_id.begin(), connection_id.end(),
                    [](std::uint8_t byte) { return byte != 0; })) {
      error.clear();
      return true;
    }
  }
  error = "BCryptGenRandom returned an invalid all-zero connection ID";
  return false;
}

bool equal_connection_id(const ConnectionId& left,
                         const ConnectionId& right) noexcept {
  return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

bool is_zero_connection_id(const ConnectionId& value) noexcept {
  return std::all_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte == 0; });
}

bool valid_control_message(const MessageHeader& header, MessageType type,
                           const ConnectionId& connection_id,
                           std::uint64_t minimum_sequence,
                           std::uint64_t correlation_id,
                           std::uint32_t expected_payload_bytes,
                           std::string& error) {
  if (header.message_type != type) {
    error = "unexpected control message type";
    return false;
  }
  if (header.flags != 0 ||
      header.payload_bytes != expected_payload_bytes) {
    error = "control message flags or payload length is invalid";
    return false;
  }
  if (!equal_connection_id(header.connection_id, connection_id)) {
    error = "control message connection ID does not match";
    return false;
  }
  if (header.message_sequence <= minimum_sequence) {
    error = "control message sequence did not increase";
    return false;
  }
  if (header.correlation_id != correlation_id) {
    error = "control message correlation ID does not match";
    return false;
  }
  error.clear();
  return true;
}

} // namespace

namespace {

struct TokenIdentity {
  std::vector<std::byte> user_sid;
  std::vector<std::byte> logon_sid;
};

bool query_token_information(HANDLE token, TOKEN_INFORMATION_CLASS info_class,
                             std::vector<std::byte>& storage,
                             std::string& error) {
  DWORD required = 0;
  if (GetTokenInformation(token, info_class, nullptr, 0, &required) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
    error = windows_error("GetTokenInformation(size)", GetLastError());
    return false;
  }
  storage.assign(required, std::byte{0});
  if (!GetTokenInformation(token, info_class, storage.data(), required,
                           &required)) {
    error = windows_error("GetTokenInformation", GetLastError());
    return false;
  }
  return true;
}

bool copy_sid(PSID sid, std::vector<std::byte>& destination,
              std::string& error) {
  if (sid == nullptr || !IsValidSid(sid)) {
    error = "Windows access token contains an invalid SID";
    return false;
  }
  const DWORD length = GetLengthSid(sid);
  destination.assign(length, std::byte{0});
  if (!CopySid(length, destination.data(), sid)) {
    error = windows_error("CopySid", GetLastError());
    return false;
  }
  return true;
}

bool query_token_identity(HANDLE token, TokenIdentity& identity,
                          std::string& error) {
  std::vector<std::byte> user_storage;
  if (!query_token_information(token, TokenUser, user_storage, error)) {
    return false;
  }
  const auto* token_user =
      reinterpret_cast<const TOKEN_USER*>(user_storage.data());
  if (!copy_sid(token_user->User.Sid, identity.user_sid, error)) return false;

  std::vector<std::byte> groups_storage;
  if (!query_token_information(token, TokenGroups, groups_storage, error)) {
    return false;
  }
  const auto* groups =
      reinterpret_cast<const TOKEN_GROUPS*>(groups_storage.data());
  identity.logon_sid.clear();
  for (DWORD index = 0; index < groups->GroupCount; ++index) {
    const SID_AND_ATTRIBUTES& group = groups->Groups[index];
    if ((group.Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
      if (!copy_sid(group.Sid, identity.logon_sid, error)) return false;
      break;
    }
  }
  if (identity.logon_sid.empty()) {
    error = "Windows access token does not contain a logon SID";
    return false;
  }
  error.clear();
  return true;
}

bool current_process_identity(TokenIdentity& identity, std::string& error) {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    error = windows_error("OpenProcessToken", GetLastError());
    return false;
  }
  UniqueHandle token(raw_token);
  return query_token_identity(token.get(), identity, error);
}

bool resolve_frame_server_service_sid(std::vector<std::byte>& sid,
                                      std::string& error) {
  sid.clear();
  DWORD sid_bytes = 0;
  DWORD domain_characters = 0;
  SID_NAME_USE sid_type = SidTypeUnknown;
  if (LookupAccountNameW(nullptr, kFrameServerAccountName, nullptr, &sid_bytes,
                         nullptr, &domain_characters, &sid_type)) {
    error = "LookupAccountName(FrameServer size) unexpectedly succeeded";
    return false;
  }
  const DWORD size_status = GetLastError();
  if (size_status != ERROR_INSUFFICIENT_BUFFER || sid_bytes == 0) {
    error = windows_error("LookupAccountName(FrameServer size)", size_status);
    return false;
  }

  sid.assign(sid_bytes, std::byte{0});
  std::vector<wchar_t> domain(std::max<DWORD>(domain_characters, 1U), L'\0');
  DWORD sid_capacity = sid_bytes;
  DWORD domain_capacity = static_cast<DWORD>(domain.size());
  if (!LookupAccountNameW(nullptr, kFrameServerAccountName, sid.data(),
                          &sid_capacity, domain.data(), &domain_capacity,
                          &sid_type)) {
    error = windows_error("LookupAccountName(FrameServer)", GetLastError());
    sid.clear();
    return false;
  }
  if (!IsValidSid(sid.data()) || sid_capacity == 0) {
    error = "NT SERVICE\\FrameServer resolved to an invalid SID";
    sid.clear();
    return false;
  }
  sid.resize(sid_capacity);
  error.clear();
  return true;
}

bool parse_string_sid(std::wstring_view text, std::vector<std::byte>& sid,
                      std::string& error) {
  sid.clear();
  if (text.empty()) {
    error = "Producer identity user SID is empty";
    return false;
  }
  const std::wstring owned_text(text);
  PSID raw_sid = nullptr;
  if (!ConvertStringSidToSidW(owned_text.c_str(), &raw_sid)) {
    error = windows_error("ConvertStringSidToSid(EngineUserSid)",
                          GetLastError());
    return false;
  }
  LocalAllocation raw_sid_owner(reinterpret_cast<HLOCAL>(raw_sid));
  return copy_sid(raw_sid, sid, error);
}

bool query_token_integrity_rid(HANDLE token, DWORD& integrity_rid,
                               std::string& error) {
  integrity_rid = 0;
  std::vector<std::byte> storage;
  if (!query_token_information(token, TokenIntegrityLevel, storage, error)) {
    return false;
  }
  const auto* label =
      reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
  PSID sid = label->Label.Sid;
  if (sid == nullptr || !IsValidSid(sid)) {
    error = "Producer token contains an invalid integrity SID";
    return false;
  }
  const UCHAR count = *GetSidSubAuthorityCount(sid);
  if (count == 0) {
    error = "Producer token integrity SID has no RID";
    return false;
  }
  integrity_rid = *GetSidSubAuthority(sid, count - 1);
  error.clear();
  return true;
}

bool token_has_enabled_group_sid(HANDLE token, PSID expected_sid,
                                 bool& present, std::string& error) {
  present = false;
  if (expected_sid == nullptr || !IsValidSid(expected_sid)) {
    error = "Expected service SID is invalid";
    return false;
  }
  std::vector<std::byte> groups_storage;
  if (!query_token_information(token, TokenGroups, groups_storage, error)) {
    return false;
  }
  const auto* groups =
      reinterpret_cast<const TOKEN_GROUPS*>(groups_storage.data());
  for (DWORD index = 0; index < groups->GroupCount; ++index) {
    const SID_AND_ATTRIBUTES& group = groups->Groups[index];
    if (group.Sid == nullptr || !IsValidSid(group.Sid)) {
      error = "Windows access token contains an invalid group SID";
      return false;
    }
    if (EqualSid(group.Sid, expected_sid) &&
        (group.Attributes & SE_GROUP_ENABLED) != 0 &&
        (group.Attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0) {
      present = true;
      break;
    }
  }
  error.clear();
  return true;
}

bool query_frame_server_process_id(DWORD& process_id, std::string& error) {
  process_id = 0;
  UniqueServiceHandle manager(
      OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager.valid()) {
    error = windows_error("OpenSCManager(FrameServer)", GetLastError());
    return false;
  }
  UniqueServiceHandle service(OpenServiceW(
      manager.get(), kFrameServerServiceName, SERVICE_QUERY_STATUS));
  if (!service.valid()) {
    error = windows_error("OpenService(FrameServer)", GetLastError());
    return false;
  }
  SERVICE_STATUS_PROCESS status{};
  DWORD returned = 0;
  if (!QueryServiceStatusEx(
          service.get(), SC_STATUS_PROCESS_INFO,
          reinterpret_cast<LPBYTE>(&status), sizeof(status), &returned)) {
    error = windows_error("QueryServiceStatusEx(FrameServer)", GetLastError());
    return false;
  }
  if (status.dwCurrentState != SERVICE_RUNNING ||
      status.dwProcessId == 0) {
    error = "FrameServer service is not running with a valid process ID";
    return false;
  }
  process_id = status.dwProcessId;
  error.clear();
  return true;
}

bool sid_to_string(const std::vector<std::byte>& sid, std::wstring& output,
                   std::string& error) {
  wchar_t* raw = nullptr;
  if (!ConvertSidToStringSidW(
          const_cast<PSID>(static_cast<const void*>(sid.data())), &raw)) {
    error = windows_error("ConvertSidToStringSid", GetLastError());
    return false;
  }
  LocalAllocation owner(raw);
  output.assign(raw);
  error.clear();
  return true;
}

bool build_pipe_security_descriptor(bool production_policy,
                                    PSECURITY_DESCRIPTOR& descriptor,
                                    std::string& error) {
  descriptor = nullptr;
  std::wstring sddl;
  if (production_policy) {
    std::vector<std::byte> frame_server_sid;
    if (!resolve_frame_server_service_sid(frame_server_sid, error)) {
      return false;
    }
    std::wstring frame_server_sid_string;
    if (!sid_to_string(frame_server_sid, frame_server_sid_string, error)) {
      return false;
    }
    // The canonical endpoint admits only SYSTEM and the concrete FrameServer
    // service SID. LocalService by itself and the interactive logon SID are
    // intentionally absent, so neither can occupy the production pipe.
    sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + frame_server_sid_string + L")";
  } else {
    TokenIdentity identity;
    if (!current_process_identity(identity, error)) return false;

    std::wstring logon_sid;
    if (!sid_to_string(identity.logon_sid, logon_sid, error)) {
      return false;
    }

    // Non-production routes retain the broad W4b-2a loopback policy used by
    // diagnostics and transport tests.
    sddl =
        L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;" + logon_sid + L")";
  }
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    error = windows_error("ConvertStringSecurityDescriptor", GetLastError());
    return false;
  }
  error.clear();
  return true;
}

bool sid_is_well_known(PSID sid, WELL_KNOWN_SID_TYPE type,
                       std::string& error) {
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  DWORD size = static_cast<DWORD>(storage.size());
  if (!CreateWellKnownSid(type, nullptr, storage.data(), &size)) {
    error = windows_error("CreateWellKnownSid", GetLastError());
    return false;
  }
  error.clear();
  return EqualSid(sid, storage.data()) != FALSE;
}

struct SidAceSummary {
  ACCESS_MASK allowed{0};
  ACCESS_MASK denied{0};
};

bool summarize_sid_aces(PACL dacl, PSID sid, SidAceSummary& summary,
                        const char* operation, std::string& error) {
  summary = {};
  if (dacl == nullptr || !IsValidAcl(dacl)) {
    error = windows_error(operation, ERROR_INVALID_SECURITY_DESCR);
    return false;
  }
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (!GetAce(dacl, index, &raw_ace)) {
      error = windows_error(operation, GetLastError());
      return false;
    }
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    ACCESS_MASK mask = 0;
    PSID ace_sid = nullptr;
    if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
      const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
      mask = ace->Mask;
      ace_sid = const_cast<DWORD*>(&ace->SidStart);
      if (IsValidSid(ace_sid) && EqualSid(ace_sid, sid)) {
        summary.allowed |= mask;
      }
    } else if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
      const auto* ace = static_cast<const ACCESS_DENIED_ACE*>(raw_ace);
      mask = ace->Mask;
      ace_sid = const_cast<DWORD*>(&ace->SidStart);
      if (IsValidSid(ace_sid) && EqualSid(ace_sid, sid)) {
        summary.denied |= mask;
      }
    }
  }
  error.clear();
  return true;
}

bool grant_kernel_object_access(HANDLE object, PSID service_sid,
                                 ACCESS_MASK requested_access,
                                const char* object_name,
                                std::string& error) {
  PACL current_dacl = nullptr;
  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  DWORD status = GetSecurityInfo(
      object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
      &current_dacl, nullptr, &raw_descriptor);
  if (status != ERROR_SUCCESS) {
    error = windows_error(
        (std::string("GetSecurityInfo(") + object_name + " DACL)").c_str(),
        status);
    return false;
  }
  LocalAllocation descriptor_owner(
      reinterpret_cast<HLOCAL>(raw_descriptor));

  const std::string inspect_operation =
      std::string("inspect ") + object_name + " DACL";
  SidAceSummary current;
  if (!summarize_sid_aces(current_dacl, service_sid, current,
                           inspect_operation.c_str(), error)) {
    return false;
  }
  if ((current.denied & requested_access) != 0) {
    error = windows_error(
        (std::string("FrameServer service SID denied by ") + object_name +
         " DACL")
            .c_str(),
        ERROR_ACCESS_DENIED);
    return false;
  }
  if ((current.allowed & ~requested_access) != 0) {
    error = windows_error(
        (std::string("FrameServer service SID has excessive ") + object_name +
         " access")
            .c_str(),
        ERROR_ACCESS_DENIED);
    return false;
  }
  if ((current.allowed & requested_access) == requested_access) {
    error.clear();
    return true;
  }

  EXPLICIT_ACCESSW entry{};
  entry.grfAccessPermissions = requested_access;
  entry.grfAccessMode = GRANT_ACCESS;
  entry.grfInheritance = NO_INHERITANCE;
  BuildTrusteeWithSidW(&entry.Trustee, service_sid);
  entry.Trustee.TrusteeType = TRUSTEE_IS_USER;

  PACL updated_dacl = nullptr;
  status = SetEntriesInAclW(1, &entry, current_dacl, &updated_dacl);
  if (status != ERROR_SUCCESS) {
    error = windows_error(
        (std::string("SetEntriesInAcl(") + object_name + " DACL)").c_str(),
        status);
    return false;
  }
  LocalAllocation updated_dacl_owner(
      reinterpret_cast<HLOCAL>(updated_dacl));
  status = SetSecurityInfo(object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                           nullptr, nullptr, updated_dacl, nullptr);
  if (status != ERROR_SUCCESS) {
    error = windows_error(
        (std::string("SetSecurityInfo(") + object_name + " DACL)").c_str(),
        status);
    return false;
  }

  PACL verified_dacl = nullptr;
  PSECURITY_DESCRIPTOR raw_verified_descriptor = nullptr;
  status = GetSecurityInfo(
      object, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
      &verified_dacl, nullptr, &raw_verified_descriptor);
  if (status != ERROR_SUCCESS) {
    error = windows_error(
        (std::string("verify GetSecurityInfo(") + object_name + " DACL)")
            .c_str(),
        status);
    return false;
  }
  LocalAllocation verified_descriptor_owner(
      reinterpret_cast<HLOCAL>(raw_verified_descriptor));
  SidAceSummary verified;
  const std::string verify_operation =
      std::string("verify ") + object_name + " DACL";
  if (!summarize_sid_aces(verified_dacl, service_sid, verified,
                           verify_operation.c_str(), error)) {
    return false;
  }
  if ((verified.denied & requested_access) != 0 ||
      (verified.allowed & requested_access) != requested_access ||
      (verified.allowed & ~requested_access) != 0) {
    error = windows_error(verify_operation.c_str(), ERROR_ACCESS_DENIED);
    return false;
  }
  error.clear();
  return true;
}

bool prepare_engine_peer_query_access(std::string& error) {
  std::vector<std::byte> frame_server_sid;
  if (!resolve_frame_server_service_sid(frame_server_sid, error)) return false;

  // Use real handles with only the standard DACL rights needed below. Opening
  // both first avoids changing either object when handle preparation fails.
  UniqueHandle process(OpenProcess(READ_CONTROL | WRITE_DAC, FALSE,
                                   GetCurrentProcessId()));
  if (!process.valid()) {
    error = windows_error("OpenProcess(READ_CONTROL|WRITE_DAC)",
                          GetLastError());
    return false;
  }
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), READ_CONTROL | WRITE_DAC,
                        &raw_token)) {
    error = windows_error("OpenProcessToken(READ_CONTROL|WRITE_DAC)",
                          GetLastError());
    return false;
  }
  UniqueHandle token(raw_token);

  PSID service_sid = frame_server_sid.data();
  if (!grant_kernel_object_access(
          process.get(), service_sid, PROCESS_QUERY_LIMITED_INFORMATION,
          "process", error)) {
    return false;
  }
  // This changes only the current primary token object's kernel DACL. It never
  // reads or writes TokenDefaultDacl, which controls defaults for future objects.
  if (!grant_kernel_object_access(token.get(), service_sid, TOKEN_QUERY,
                                  "primary token", error)) {
    return false;
  }
  error.clear();
  return true;
}

class ImpersonationGuard {
 public:
  ImpersonationGuard() noexcept = default;
  ~ImpersonationGuard() {
    if (active_) (void)RevertToSelf();
  }
  void activate() noexcept { active_ = true; }
  bool revert(std::string& error) noexcept {
    if (!active_) return true;
    if (!RevertToSelf()) {
      error = windows_error("RevertToSelf", GetLastError());
      return false;
    }
    active_ = false;
    return true;
  }

 private:
  bool active_{false};
};

bool verify_connected_client(HANDLE pipe, bool production_policy,
                             std::uint32_t& process_id, std::string& error) {
  process_id = 0;
  TokenIdentity current;
  std::vector<std::byte> frame_server_sid;
  if (production_policy) {
    if (!resolve_frame_server_service_sid(frame_server_sid, error)) return false;
  } else {
    // Capture the server identity before impersonating. An
    // identification-level client token cannot be used to open the process
    // token while impersonated.
    if (!current_process_identity(current, error)) return false;
  }
  if (!ImpersonateNamedPipeClient(pipe)) {
    error = windows_error("ImpersonateNamedPipeClient", GetLastError());
    return false;
  }
  ImpersonationGuard impersonation;
  impersonation.activate();

  bool accepted = false;
  std::string verification_error;
  HANDLE raw_token = nullptr;
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
    verification_error = windows_error("OpenThreadToken", GetLastError());
  } else {
    UniqueHandle token(raw_token);
    TokenIdentity peer;
    if (query_token_identity(token.get(), peer, verification_error)) {
      PSID peer_user = peer.user_sid.data();
      std::string well_known_error;
      const bool local_service =
          sid_is_well_known(peer_user, WinLocalServiceSid, well_known_error);
      if (!well_known_error.empty()) {
        verification_error = well_known_error;
      } else if (production_policy) {
        bool has_frame_server_sid = false;
        if (!token_has_enabled_group_sid(token.get(), frame_server_sid.data(),
                                         has_frame_server_sid,
                                         verification_error)) {
          accepted = false;
        } else {
          accepted = local_service && has_frame_server_sid;
          if (!accepted) {
            verification_error =
                "named-pipe client is not the FrameServer service identity";
          }
        }
      } else {
        const bool local_system =
            sid_is_well_known(peer_user, WinLocalSystemSid, well_known_error);
        if (!well_known_error.empty()) {
          verification_error = well_known_error;
        } else {
          const bool same_user =
              EqualSid(peer.user_sid.data(), current.user_sid.data()) != FALSE;
          const bool same_logon =
              EqualSid(peer.logon_sid.data(), current.logon_sid.data()) != FALSE;
          accepted = local_service || local_system || (same_user && same_logon);
          if (!accepted) {
            verification_error =
                "named-pipe client identity is not permitted";
          }
        }
      }
    }
  }

  std::string revert_error;
  if (!impersonation.revert(revert_error)) {
    error = revert_error;
    return false;
  }
  if (!accepted) {
    error = verification_error.empty()
                ? "named-pipe client identity could not be verified"
                : verification_error;
    return false;
  }

  ULONG client_process_id = 0;
  if (!GetNamedPipeClientProcessId(pipe, &client_process_id)) {
    error = windows_error("GetNamedPipeClientProcessId", GetLastError());
    return false;
  }
  if (production_policy) {
    DWORD frame_server_process_id = 0;
    if (!query_frame_server_process_id(frame_server_process_id, error)) {
      return false;
    }
    if (client_process_id != frame_server_process_id) {
      error = "named-pipe client PID does not match the running FrameServer "
              "service";
      return false;
    }
  }
  process_id = static_cast<std::uint32_t>(client_process_id);
  error.clear();
  return true;
}

bool query_token_session_id(HANDLE token, DWORD& session_id,
                            std::string& error) {
  DWORD returned = 0;
  if (!GetTokenInformation(token, TokenSessionId, &session_id,
                           sizeof(session_id), &returned)) {
    error = windows_error("GetTokenInformation(TokenSessionId)",
                          GetLastError());
    return false;
  }
  error.clear();
  return true;
}

bool verify_connected_server(HANDLE pipe, bool production_policy,
                             std::uint32_t& process_id,
                             std::wstring* verified_producer_user_sid,
                             std::string& error) {
  process_id = 0;
  if (verified_producer_user_sid != nullptr) {
    verified_producer_user_sid->clear();
  }
  ULONG raw_process_id = 0;
  if (!GetNamedPipeServerProcessId(pipe, &raw_process_id)) {
    error = windows_error("GetNamedPipeServerProcessId", GetLastError());
    return false;
  }
  if (raw_process_id == 0) {
    error = "GetNamedPipeServerProcessId returned process ID zero";
    return false;
  }
  process_id = static_cast<std::uint32_t>(raw_process_id);

  // This branch exists solely for non-production loopback transport tests. A
  // canonical endpoint must never bypass its installed path/hash policy.
  if (!production_policy && raw_process_id == GetCurrentProcessId()) {
    error.clear();
    return true;
  }

  UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   raw_process_id));
  if (!process.valid()) {
    error = windows_error("OpenProcess(control server)", GetLastError());
    return false;
  }

  std::vector<wchar_t> path(32768, L'\0');
  DWORD path_length = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, path.data(),
                                  &path_length)) {
    error = windows_error("QueryFullProcessImageName(control server)",
                          GetLastError());
    return false;
  }
  const std::wstring full_path(path.data(), path_length);
  const std::size_t separator = full_path.find_last_of(L"\\/");
  const std::wstring basename(
      separator == std::wstring_view::npos ? full_path
                                           : full_path.substr(separator + 1));
  if (!production_policy &&
      _wcsicmp(basename.c_str(), L"vividcam_engine.exe") != 0) {
    error = "Named-pipe server image is not vividcam_engine.exe";
    return false;
  }

  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token)) {
    error = windows_error("OpenProcessToken(control server)", GetLastError());
    return false;
  }
  UniqueHandle token(raw_token);
  TokenIdentity identity;
  if (!query_token_identity(token.get(), identity, error)) return false;

  std::string well_known_error;
  const bool local_service = sid_is_well_known(
      identity.user_sid.data(), WinLocalServiceSid, well_known_error);
  if (!well_known_error.empty()) {
    error = well_known_error;
    return false;
  }
  const bool local_system = sid_is_well_known(
      identity.user_sid.data(), WinLocalSystemSid, well_known_error);
  if (!well_known_error.empty()) {
    error = well_known_error;
    return false;
  }
  const bool network_service = sid_is_well_known(
      identity.user_sid.data(), WinNetworkServiceSid, well_known_error);
  if (!well_known_error.empty()) {
    error = well_known_error;
    return false;
  }
  if (local_service || local_system || network_service) {
    error = "Named-pipe server must run as a normal user principal";
    return false;
  }

  DWORD token_session_id = 0;
  if (!query_token_session_id(token.get(), token_session_id, error)) {
    return false;
  }
  ULONG pipe_session_id = 0;
  if (!GetNamedPipeServerSessionId(pipe, &pipe_session_id)) {
    error = windows_error("GetNamedPipeServerSessionId", GetLastError());
    return false;
  }
  if (token_session_id != pipe_session_id) {
    error = "Named-pipe server token and pipe session IDs do not match";
    return false;
  }

  if (production_policy) {
    ProducerIdentityManifest manifest;
    if (!load_installed_vividcam_producer_identity(manifest, error)) {
      return false;
    }
    std::vector<std::byte> manifest_user_sid;
    if (!parse_string_sid(manifest.engine_user_sid, manifest_user_sid, error)) {
      return false;
    }
    if (EqualSid(identity.user_sid.data(), manifest_user_sid.data()) == FALSE) {
      error = "Named-pipe server user does not match the installed producer "
              "identity";
      return false;
    }
    const DWORD active_console_session = WTSGetActiveConsoleSessionId();
    if (active_console_session == 0xffffffffU || token_session_id == 0 ||
        token_session_id != active_console_session) {
      error = "Named-pipe server is not running in the active console user "
              "session";
      return false;
    }
    TOKEN_ELEVATION_TYPE elevation_type = TokenElevationTypeDefault;
    DWORD elevation_bytes = 0;
    if (!GetTokenInformation(token.get(), TokenElevationType, &elevation_type,
                             sizeof(elevation_type), &elevation_bytes)) {
      error = windows_error("GetTokenInformation(TokenElevationType)",
                            GetLastError());
      return false;
    }
    if (elevation_bytes != sizeof(elevation_type)) {
      error = "Producer token returned an invalid elevation type length";
      return false;
    }
    if (elevation_type == TokenElevationTypeFull) {
      error = "Named-pipe server must not run with an elevated token";
      return false;
    }
    DWORD integrity_rid = 0;
    if (!query_token_integrity_rid(token.get(), integrity_rid, error)) {
      return false;
    }
    if (integrity_rid > SECURITY_MANDATORY_MEDIUM_RID) {
      error = "Named-pipe server must not run with elevated integrity";
      return false;
    }
    std::wstring expected_package_path;
    if (!current_module_sibling_vividcam_engine_path(expected_package_path,
                                                      error)) {
      return false;
    }
    if (!verify_vividcam_producer_image(manifest, full_path,
                                        expected_package_path, error)) {
      return false;
    }
    if (verified_producer_user_sid != nullptr) {
      *verified_producer_user_sid = manifest.engine_user_sid;
    }
  }

  // Production additionally pins the observed image to the installed path and
  // SHA-256 manifest. Non-production routes intentionally retain only the
  // basename/token/session gate used by diagnostics.
  error.clear();
  return true;
}

IoResult connect_server_pipe(HANDLE pipe, HANDLE stop_event,
                             std::string& error) {
  UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event.valid()) {
    error = windows_error("CreateEvent(pipe connect)", GetLastError());
    return IoResult::Failed;
  }
  OVERLAPPED overlapped{};
  overlapped.hEvent = event.get();
  if (ConnectNamedPipe(pipe, &overlapped)) {
    error.clear();
    return IoResult::Complete;
  }
  const DWORD status = GetLastError();
  if (status == ERROR_PIPE_CONNECTED) {
    error.clear();
    return IoResult::Complete;
  }
  if (status != ERROR_IO_PENDING) {
    error = windows_error("ConnectNamedPipe", status);
    return IoResult::Failed;
  }
  DWORD transferred = 0;
  return finish_overlapped(pipe, stop_event, overlapped,
                           Clock::time_point::max(), transferred, error);
}

void update_last_error(ControlChannelTransportSnapshot& snapshot,
                       const std::string& error) {
  if (!error.empty()) snapshot.last_error = error;
}

bool hash_route(std::wstring_view route, std::array<std::uint8_t, 32>& digest,
                std::string& error) {
  if (route.empty()) {
    error = "VIVIDCAM control route must not be empty";
    return false;
  }
  static_assert(sizeof(wchar_t) == 2,
                "Windows control routes require UTF-16 wchar_t");
  if (route.size() >
      static_cast<std::size_t>(std::numeric_limits<ULONG>::max()) /
          sizeof(wchar_t)) {
    error = "VIVIDCAM control route is too long";
    return false;
  }

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCryptOpenAlgorithmProvider(SHA-256)", status);
    return false;
  }

  DWORD object_bytes = 0;
  DWORD result_bytes = 0;
  status = BCryptGetProperty(
      algorithm, BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
      &result_bytes, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    error = ntstatus_error("BCryptGetProperty(object length)", status);
    return false;
  }
  DWORD digest_bytes = 0;
  status = BCryptGetProperty(
      algorithm, BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&digest_bytes), sizeof(digest_bytes),
      &result_bytes, 0);
  if (!BCRYPT_SUCCESS(status) || digest_bytes != digest.size()) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
    error = BCRYPT_SUCCESS(status)
                ? "BCrypt SHA-256 digest length is not 32 bytes"
                : ntstatus_error("BCryptGetProperty(hash length)", status);
    return false;
  }

  std::vector<std::uint8_t> hash_object(object_bytes, 0);
  BCRYPT_HASH_HANDLE hash = nullptr;
  status = BCryptCreateHash(algorithm, &hash, hash_object.data(), object_bytes,
                            nullptr, 0, 0);
  if (BCRYPT_SUCCESS(status)) {
    status = BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(
            const_cast<wchar_t*>(route.data())),
        static_cast<ULONG>(route.size() * sizeof(wchar_t)), 0);
  }
  if (BCRYPT_SUCCESS(status)) {
    status = BCryptFinishHash(hash, digest.data(),
                              static_cast<ULONG>(digest.size()), 0);
  }
  if (hash != nullptr) BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCrypt SHA-256 route digest", status);
    return false;
  }
  error.clear();
  return true;
}

bool make_route_digest(std::wstring_view route, std::wstring& route_digest,
                       std::string& error) {
  route_digest.clear();
  std::array<std::uint8_t, 32> digest{};
  if (!hash_route(route, digest, error)) return false;

  constexpr wchar_t hex[] = L"0123456789abcdef";
  route_digest.reserve(digest.size() * 2U);
  for (std::uint8_t byte : digest) {
    route_digest.push_back(hex[byte >> 4U]);
    route_digest.push_back(hex[byte & 0x0fU]);
  }
  error.clear();
  return true;
}

} // namespace

bool make_vividcam_control_pipe_name(std::wstring_view route,
                                     std::wstring& pipe_name,
                                     std::string& error) {
  pipe_name.clear();
  std::wstring route_digest;
  if (!make_route_digest(route, route_digest, error)) return false;
  pipe_name.assign(kPipePrefix);
  pipe_name.append(route_digest);
  error.clear();
  return true;
}

bool find_registered_vividcam_control_route(std::wstring& route,
                                            std::string& error) {
  route.clear();
  const HRESULT com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool owns_com = SUCCEEDED(com_status);
  if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
    error = hresult_error("CoInitializeEx", com_status);
    return false;
  }

  const HRESULT startup_status = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(startup_status)) {
    if (owns_com) CoUninitialize();
    error = hresult_error("MFStartup", startup_status);
    return false;
  }

  IMFAttributes* attributes = nullptr;
  HRESULT status = MFCreateAttributes(&attributes, 1);
  if (SUCCEEDED(status)) {
    status = attributes->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  }

  IMFActivate** devices = nullptr;
  UINT32 device_count = 0;
  if (SUCCEEDED(status)) {
    status = MFEnumDeviceSources(attributes, &devices, &device_count);
  }
  if (attributes != nullptr) attributes->Release();

  if (SUCCEEDED(status)) {
    constexpr wchar_t target_name[] = L"VIVIDCAM Virtual Camera";
    constexpr int target_name_length =
        static_cast<int>(std::size(target_name) - 1U);
    for (UINT32 index = 0; index < device_count; ++index) {
      wchar_t* friendly_name = nullptr;
      UINT32 friendly_length = 0;
      const HRESULT name_status = devices[index]->GetAllocatedString(
          MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendly_name,
          &friendly_length);
      const bool matches =
          SUCCEEDED(name_status) && friendly_name != nullptr &&
          friendly_length >= static_cast<UINT32>(target_name_length) &&
          CompareStringOrdinal(friendly_name, target_name_length, target_name,
                               target_name_length, TRUE) == CSTR_EQUAL;
      CoTaskMemFree(friendly_name);
      if (matches) {
        wchar_t* symbolic_link = nullptr;
        UINT32 link_length = 0;
        const HRESULT link_status = devices[index]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
            &symbolic_link, &link_length);
        if (SUCCEEDED(link_status) && symbolic_link != nullptr &&
            link_length != 0) {
          route.assign(symbolic_link, link_length);
        } else if (FAILED(link_status)) {
          status = link_status;
        }
        CoTaskMemFree(symbolic_link);
        if (!route.empty() || FAILED(status)) break;
      }
    }
  }
  for (UINT32 index = 0; index < device_count; ++index) {
    devices[index]->Release();
  }
  CoTaskMemFree(devices);
  MFShutdown();
  if (owns_com) CoUninitialize();

  if (FAILED(status)) {
    error = hresult_error("MFEnumDeviceSources(control route)", status);
    return false;
  }
  if (route.empty()) {
    error = "Registered VIVIDCAM virtual camera was not found";
    return false;
  }
  // Registration and its symbolic link are validated above, but rendezvous is
  // intentionally independent of Windows' per-activation link formatting.
  route.assign(kVividCamPrimaryControlRoute);
  error.clear();
  return true;
}

namespace {

producer_ipc::OpenStreamPayload fixed_open_stream_payload() {
  static_assert(kCpuFrameNv12Bytes <=
                std::numeric_limits<std::uint32_t>::max());
  producer_ipc::OpenStreamPayload payload;
  payload.width = kCpuFrameWidth;
  payload.height = kCpuFrameHeight;
  payload.frame_rate_numerator = 60;
  payload.frame_rate_denominator = 1;
  payload.pixel_format = producer_ipc::FramePixelFormat::Nv12;
  payload.plane0_stride_bytes = kCpuFrameYStrideBytes;
  payload.plane1_stride_bytes = kCpuFrameUvStrideBytes;
  payload.frame_bytes = static_cast<std::uint32_t>(kCpuFrameNv12Bytes);
  return payload;
}

producer_ipc::TransportOfferPayload fixed_transport_offer_payload() {
  static_assert(cpu_frame_mailbox_layout::kHeaderBytes <=
                std::numeric_limits<std::uint32_t>::max());
  producer_ipc::TransportOfferPayload payload;
  payload.frame_capacity_bytes =
      static_cast<std::uint32_t>(kCpuFrameNv12Bytes);
  payload.mapping_header_bytes =
      static_cast<std::uint32_t>(cpu_frame_mailbox_layout::kHeaderBytes);
  payload.mapping_capacity_bytes = cpu_frame_mailbox_layout::kMappingBytes;
  return payload;
}

producer_ipc::TransportDescriptorPayload transport_descriptor_for(
    const producer_ipc::TransportOfferPayload& offer) {
  producer_ipc::TransportDescriptorPayload descriptor;
  descriptor.transport_kind = offer.transport_kind;
  descriptor.layout_major = offer.layout_major;
  descriptor.layout_minor = offer.layout_minor;
  descriptor.slot_count = offer.slot_count;
  descriptor.mapping_header_bytes = offer.mapping_header_bytes;
  descriptor.frame_capacity_bytes = offer.frame_capacity_bytes;
  descriptor.mapping_capacity_bytes = offer.mapping_capacity_bytes;
  descriptor.flags = offer.flags;
  descriptor.reserved = offer.reserved;
  return descriptor;
}

bool negotiation_payload_succeeded(
    producer_ipc::NegotiationPayloadError status,
    std::string_view operation, std::string& error) {
  if (status == producer_ipc::NegotiationPayloadError::None) {
    error.clear();
    return true;
  }
  error.assign(operation);
  error.append(": ");
  error.append(producer_ipc::negotiation_payload_error_message(status));
  return false;
}

CpuFrameMailboxOptions mailbox_options(
    bool production_policy, const std::wstring& route_digest,
    const ConnectionId& connection_id,
    const std::wstring& producer_user_sid) {
  CpuFrameMailboxOptions options;
  options.scope = production_policy
                      ? CpuFrameMailboxScope::ProductionGlobal
                      : CpuFrameMailboxScope::NonProductionLocal;
  options.route_digest = route_digest;
  options.connection_id = connection_id;
  options.producer_user_sid = producer_user_sid;
  return options;
}

} // namespace

class ProducerControlServer::Impl {
 public:
  bool start(std::wstring route, std::string engine_instance_id,
             std::string& error) {
    // Lifecycle operations always acquire lifecycle_mutex_ before mutex_. The
    // worker never acquires lifecycle_mutex_, so stop can hold it through join.
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    const bool production_policy = uses_production_peer_policy(route);
    std::wstring pipe_name;
    if (!make_vividcam_control_pipe_name(route, pipe_name, error)) return false;
    std::wstring route_digest;
    if (!make_route_digest(route, route_digest, error)) return false;
    if (engine_instance_id.empty()) {
      error = "Producer engine instance ID must not be empty";
      return false;
    }

    std::scoped_lock state_lock(mutex_);
    if (worker_.joinable()) {
      error = "Producer control server is already running";
      return false;
    }
    if (!prepare_engine_peer_query_access(error)) {
      snapshot_.running = false;
      snapshot_.connected = false;
      snapshot_.last_error = error;
      return false;
    }
    if (!stop_event_.valid()) {
      stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!stop_event_.valid()) {
        error = windows_error("CreateEvent(server stop)", GetLastError());
        return false;
      }
    } else if (!ResetEvent(stop_event_.get())) {
      error = windows_error("ResetEvent(server stop)", GetLastError());
      return false;
    }

    pipe_name_ = std::move(pipe_name);
    route_digest_ = std::move(route_digest);
    engine_instance_id_ = std::move(engine_instance_id);
    production_policy_ = production_policy;
    snapshot_ = {};
    snapshot_.running = true;
    try {
      worker_ = std::thread([this] { run_guarded(); });
    } catch (const std::exception& exception) {
      snapshot_.running = false;
      snapshot_.last_error = exception.what();
      error = std::string("Could not start producer control worker: ") +
              exception.what();
      return false;
    }
    error.clear();
    return true;
  }

  void stop() noexcept {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::thread worker;
    std::shared_ptr<CpuFrameMailboxProducer> mailbox;
    {
      std::scoped_lock lock(mutex_);
      if (!worker_.joinable()) {
        snapshot_.running = false;
        snapshot_.connected = false;
        snapshot_.peer_process_id = 0;
        mailbox = std::move(active_mailbox_);
      } else {
        (void)SetEvent(stop_event_.get());
        // PipePublicationGuard clears this slot under the same mutex before
        // the owning UniqueHandle closes, so the handle cannot become stale.
        if (active_pipe_ != INVALID_HANDLE_VALUE) {
          (void)CancelIoEx(active_pipe_, nullptr);
        }
        worker = std::move(worker_);
      }
    }
    if (mailbox) mailbox->close();
    if (!worker.joinable()) return;
    if (worker.joinable()) worker.join();
    {
      std::scoped_lock lock(mutex_);
      snapshot_.running = false;
      snapshot_.connected = false;
      snapshot_.peer_process_id = 0;
      active_pipe_ = INVALID_HANDLE_VALUE;
      mailbox = std::move(active_mailbox_);
    }
    if (mailbox) mailbox->close();
  }

  ControlChannelTransportSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
  }

  CpuFramePublishResult publish_cpu_frame_for_mailbox(
      const CpuNv12Frame& frame, std::wstring_view expected_mailbox_name,
      std::string& error) {
    std::scoped_lock lock(mutex_);
    if (!active_mailbox_) {
      error = "CPU frame mailbox is not ready";
      return CpuFramePublishResult::TransportUnavailable;
    }
    if (expected_mailbox_name.empty() ||
        active_mailbox_->name() != expected_mailbox_name) {
      error = "CPU frame mailbox changed before publish";
      return CpuFramePublishResult::MailboxChanged;
    }
    if (!active_mailbox_->publish(frame, error)) {
      return CpuFramePublishResult::Failed;
    }
    error.clear();
    return CpuFramePublishResult::Published;
  }

  CpuFrameMailboxSnapshot frame_mailbox_snapshot() const {
    std::scoped_lock lock(mutex_);
    return active_mailbox_ ? active_mailbox_->snapshot()
                           : CpuFrameMailboxSnapshot{};
  }

  std::wstring frame_mailbox_name() const {
    std::scoped_lock lock(mutex_);
    return active_mailbox_ ? active_mailbox_->name() : std::wstring{};
  }

 private:
  class PipePublicationGuard {
   public:
    PipePublicationGuard(Impl& owner, HANDLE pipe) noexcept
        : owner_(owner), pipe_(pipe) {}
    ~PipePublicationGuard() { owner_.clear_pipe(pipe_); }
    PipePublicationGuard(const PipePublicationGuard&) = delete;
    PipePublicationGuard& operator=(const PipePublicationGuard&) = delete;

   private:
    Impl& owner_;
    HANDLE pipe_;
  };

  class MailboxPublicationGuard {
   public:
    explicit MailboxPublicationGuard(Impl& owner) noexcept : owner_(owner) {}
    ~MailboxPublicationGuard() { unpublish(); }
    MailboxPublicationGuard(const MailboxPublicationGuard&) = delete;
    MailboxPublicationGuard& operator=(const MailboxPublicationGuard&) = delete;

    void publish(std::shared_ptr<CpuFrameMailboxProducer> mailbox) {
      mailbox_ = std::move(mailbox);
      owner_.publish_mailbox(mailbox_);
    }

   private:
    void unpublish() noexcept {
      if (!mailbox_) return;
      owner_.clear_mailbox(mailbox_);
      mailbox_->close();
      mailbox_.reset();
    }

    Impl& owner_;
    std::shared_ptr<CpuFrameMailboxProducer> mailbox_;
  };

  void run_guarded() noexcept {
    try {
      worker_main();
    } catch (const std::exception& exception) {
      std::scoped_lock lock(mutex_);
      snapshot_.last_error =
          std::string("Producer control worker stopped: ") + exception.what();
    } catch (...) {
      std::scoped_lock lock(mutex_);
      snapshot_.last_error = "Producer control worker stopped unexpectedly";
    }
    std::shared_ptr<CpuFrameMailboxProducer> mailbox;
    {
      std::scoped_lock lock(mutex_);
      snapshot_.running = false;
      snapshot_.connected = false;
      snapshot_.peer_process_id = 0;
      // Defensive only: publication guards normally clear both resources.
      active_pipe_ = INVALID_HANDLE_VALUE;
      mailbox = std::move(active_mailbox_);
    }
    if (mailbox) mailbox->close();
  }

  void publish_pipe(HANDLE pipe) {
    std::scoped_lock lock(mutex_);
    active_pipe_ = pipe;
  }

  void clear_pipe(HANDLE pipe) {
    std::scoped_lock lock(mutex_);
    if (active_pipe_ == pipe) active_pipe_ = INVALID_HANDLE_VALUE;
    snapshot_.connected = false;
    snapshot_.peer_process_id = 0;
  }

  void publish_mailbox(
      const std::shared_ptr<CpuFrameMailboxProducer>& mailbox) {
    std::scoped_lock lock(mutex_);
    active_mailbox_ = mailbox;
  }

  void clear_mailbox(
      const std::shared_ptr<CpuFrameMailboxProducer>& mailbox) {
    std::scoped_lock lock(mutex_);
    if (active_mailbox_ == mailbox) active_mailbox_.reset();
  }

  void record_error(const std::string& error, bool protocol_error,
                    bool rejected_peer) {
    std::scoped_lock lock(mutex_);
    update_last_error(snapshot_, error);
    if (protocol_error) ++snapshot_.protocol_errors;
    if (rejected_peer) ++snapshot_.rejected_peers;
  }

  bool serve_client(HANDLE pipe, std::string& session_error) {
    MessageHeader source_hello;
    const IoResult hello_result = read_message(
        pipe, stop_event_.get(), source_hello,
        Clock::now() + kHandshakeTimeout, session_error);
    if (hello_result != IoResult::Complete) {
      if (hello_result == IoResult::ProtocolFailure) {
        record_error(session_error, true, false);
      }
      return false;
    }

    // Impersonation deliberately follows the first successful client read:
    // Windows associates ImpersonateNamedPipeClient with the security context
    // of the last message read. No Hello field is trusted before this check.
    std::uint32_t client_process_id = 0;
    if (!verify_connected_client(pipe, production_policy_, client_process_id,
                                 session_error)) {
      record_error(session_error, false, true);
      return false;
    }

    if (source_hello.message_type != MessageType::SourceHello ||
        source_hello.message_sequence != 1 ||
        source_hello.correlation_id != 0 || source_hello.flags != 0 ||
        source_hello.payload_bytes != 0 ||
        is_zero_connection_id(source_hello.connection_id)) {
      session_error = "SourceHello handshake contract is invalid";
      record_error(session_error, true, false);
      return false;
    }

    const Clock::time_point negotiation_deadline =
        Clock::now() + kHandshakeTimeout;
    MessageHeader producer_hello;
    producer_hello.message_type = MessageType::ProducerHello;
    producer_hello.message_sequence = 1;
    producer_hello.correlation_id = source_hello.message_sequence;
    producer_hello.connection_id = source_hello.connection_id;
    const IoResult write_hello = write_message(
        pipe, stop_event_.get(), producer_hello,
        negotiation_deadline, session_error);
    if (write_hello != IoResult::Complete) {
      if (write_hello == IoResult::ProtocolFailure) {
        record_error(session_error, true, false);
      }
      return false;
    }

    std::vector<std::byte> payload;
    MessageHeader open_stream_header;
    IoResult negotiation_result = read_message(
        pipe, stop_event_.get(), open_stream_header, payload,
        negotiation_deadline, session_error);
    if (negotiation_result != IoResult::Complete) {
      if (negotiation_result == IoResult::ProtocolFailure) {
        record_error(session_error, true, false);
      }
      return false;
    }
    if (!valid_control_message(
            open_stream_header, MessageType::OpenStream,
            source_hello.connection_id, source_hello.message_sequence,
            producer_hello.message_sequence,
            producer_ipc::kOpenStreamPayloadBytes, session_error)) {
      record_error(session_error, true, false);
      return false;
    }
    producer_ipc::OpenStreamPayload open_stream;
    if (!negotiation_payload_succeeded(
            producer_ipc::decode_open_stream_payload(payload, open_stream),
            "OpenStream payload is invalid", session_error)) {
      record_error(session_error, true, false);
      return false;
    }

    const producer_ipc::TransportOfferPayload offer =
        fixed_transport_offer_payload();
    if (!negotiation_payload_succeeded(
            producer_ipc::validate_transport_offer_for_open_stream(open_stream,
                                                                    offer),
            "OpenStream cannot use the CPU mailbox offer", session_error)) {
      record_error(session_error, true, false);
      return false;
    }
    std::vector<std::byte> offer_bytes;
    if (!negotiation_payload_succeeded(
            producer_ipc::encode_transport_offer_payload(offer, offer_bytes),
            "Could not encode TransportOffer", session_error)) {
      return false;
    }
    MessageHeader offer_header;
    offer_header.message_type = MessageType::TransportOffer;
    offer_header.message_sequence = 2;
    offer_header.correlation_id = open_stream_header.message_sequence;
    offer_header.connection_id = source_hello.connection_id;
    offer_header.payload_bytes = static_cast<std::uint32_t>(offer_bytes.size());
    negotiation_result = write_message(
        pipe, stop_event_.get(), offer_header, offer_bytes,
        negotiation_deadline, session_error);
    if (negotiation_result != IoResult::Complete) return false;

    MessageHeader accepted_header;
    negotiation_result = read_message(
        pipe, stop_event_.get(), accepted_header, payload,
        negotiation_deadline, session_error);
    if (negotiation_result != IoResult::Complete) {
      if (negotiation_result == IoResult::ProtocolFailure) {
        record_error(session_error, true, false);
      }
      return false;
    }
    if (!valid_control_message(
            accepted_header, MessageType::TransportAccepted,
            source_hello.connection_id, open_stream_header.message_sequence,
            offer_header.message_sequence,
            producer_ipc::kTransportDescriptorPayloadBytes, session_error)) {
      record_error(session_error, true, false);
      return false;
    }
    producer_ipc::TransportDescriptorPayload accepted_descriptor;
    if (!negotiation_payload_succeeded(
            producer_ipc::decode_transport_descriptor_payload(
                payload, accepted_descriptor),
            "TransportAccepted payload is invalid", session_error) ||
        !negotiation_payload_succeeded(
            producer_ipc::validate_transport_descriptor_for_offer(
                offer, accepted_descriptor),
            "TransportAccepted changed the CPU mailbox offer",
            session_error)) {
      record_error(session_error, true, false);
      return false;
    }

    std::wstring producer_user_sid;
    if (production_policy_) {
      TokenIdentity engine_identity;
      if (!current_process_identity(engine_identity, session_error) ||
          !sid_to_string(engine_identity.user_sid, producer_user_sid,
                         session_error)) {
        return false;
      }
    }
    auto mailbox = open_cpu_frame_mailbox_producer(
        mailbox_options(production_policy_, route_digest_,
                        source_hello.connection_id, producer_user_sid),
        session_error);
    if (!mailbox) return false;

    const producer_ipc::TransportDescriptorPayload ready_descriptor =
        transport_descriptor_for(offer);
    std::vector<std::byte> ready_bytes;
    if (!negotiation_payload_succeeded(
            producer_ipc::encode_transport_descriptor_payload(ready_descriptor,
                                                               ready_bytes),
            "Could not encode StreamReady", session_error)) {
      mailbox->close();
      return false;
    }
    MessageHeader ready_header;
    ready_header.message_type = MessageType::StreamReady;
    ready_header.message_sequence = 3;
    ready_header.correlation_id = accepted_header.message_sequence;
    ready_header.connection_id = source_hello.connection_id;
    ready_header.payload_bytes = static_cast<std::uint32_t>(ready_bytes.size());
    negotiation_result = write_message(
        pipe, stop_event_.get(), ready_header, ready_bytes,
        negotiation_deadline, session_error);
    if (negotiation_result != IoResult::Complete) {
      mailbox->close();
      return false;
    }

    MailboxPublicationGuard mailbox_publication(*this);
    mailbox_publication.publish(std::move(mailbox));
    {
      std::scoped_lock lock(mutex_);
      snapshot_.connected = true;
      snapshot_.peer_process_id = client_process_id;
      ++snapshot_.successful_handshakes;
      snapshot_.last_error.clear();
    }

    std::uint64_t server_sequence = ready_header.message_sequence;
    std::uint64_t client_sequence = accepted_header.message_sequence;
    while (!stop_requested(stop_event_.get())) {
      const DWORD waited = WaitForSingleObject(
          stop_event_.get(), static_cast<DWORD>(kHeartbeatInterval.count()));
      if (waited == WAIT_OBJECT_0) return true;
      if (waited != WAIT_TIMEOUT) {
        session_error =
            windows_error("WaitForSingleObject(heartbeat)", GetLastError());
        return false;
      }

      ++server_sequence;
      MessageHeader heartbeat;
      heartbeat.message_type = MessageType::Heartbeat;
      heartbeat.message_sequence = server_sequence;
      heartbeat.connection_id = source_hello.connection_id;
      const IoResult sent = write_message(
          pipe, stop_event_.get(), heartbeat,
          Clock::now() + kHeartbeatAckTimeout, session_error);
      if (sent != IoResult::Complete) {
        if (sent == IoResult::ProtocolFailure) {
          record_error(session_error, true, false);
        }
        return false;
      }
      {
        std::scoped_lock lock(mutex_);
        ++snapshot_.heartbeats_sent;
      }

      MessageHeader acknowledgement;
      const IoResult received = read_message(
          pipe, stop_event_.get(), acknowledgement,
          Clock::now() + kHeartbeatAckTimeout, session_error);
      if (received != IoResult::Complete) {
        if (received == IoResult::ProtocolFailure) {
          record_error(session_error, true, false);
        }
        return false;
      }
      if (!valid_control_message(
              acknowledgement, MessageType::HeartbeatAck,
              source_hello.connection_id, client_sequence, server_sequence,
              0, session_error)) {
        record_error(session_error, true, false);
        return false;
      }
      client_sequence = acknowledgement.message_sequence;
      std::scoped_lock lock(mutex_);
      ++snapshot_.heartbeat_acks;
    }
    return true;
  }

  void worker_main() {
    // The engine instance ID remains local telemetry; VCIP negotiation derives
    // its per-session mailbox name from the route digest and connection ID.
    (void)engine_instance_id_;
    while (!stop_requested(stop_event_.get())) {
      PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
      std::string error;
      if (!build_pipe_security_descriptor(production_policy_, raw_descriptor,
                                          error)) {
        record_error(error, false, false);
        if (WaitForSingleObject(stop_event_.get(),
                                static_cast<DWORD>(kServerRetryDelay.count())) ==
            WAIT_OBJECT_0) {
          break;
        }
        continue;
      }
      LocalAllocation descriptor_owner(
          reinterpret_cast<HLOCAL>(raw_descriptor));
      SECURITY_ATTRIBUTES security{};
      security.nLength = sizeof(security);
      security.lpSecurityDescriptor = raw_descriptor;
      security.bInheritHandle = FALSE;

      const DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                              FILE_FLAG_FIRST_PIPE_INSTANCE;
      const DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                              PIPE_REJECT_REMOTE_CLIENTS;
      UniqueHandle pipe(CreateNamedPipeW(
          pipe_name_.c_str(), open_mode, pipe_mode, 1, kPipeBufferBytes,
          kPipeBufferBytes, 0, &security));
      if (!pipe.valid()) {
        record_error(windows_error("CreateNamedPipe", GetLastError()), false,
                     false);
        if (WaitForSingleObject(stop_event_.get(),
                                static_cast<DWORD>(kServerRetryDelay.count())) ==
            WAIT_OBJECT_0) {
          break;
        }
        continue;
      }

      publish_pipe(pipe.get());
      // Declared after `pipe`, so it always unpublishes the raw handle before
      // UniqueHandle closes it, including exception unwinding paths.
      PipePublicationGuard pipe_publication(*this, pipe.get());
      {
        std::scoped_lock lock(mutex_);
        ++snapshot_.connection_attempts;
      }
      const IoResult connected =
          connect_server_pipe(pipe.get(), stop_event_.get(), error);
      if (connected == IoResult::Complete) {
        std::string session_error;
        const bool clean_stop = serve_client(pipe.get(), session_error);
        if (!clean_stop && !session_error.empty() &&
            !stop_requested(stop_event_.get())) {
          record_error(session_error, false, false);
        }
        (void)DisconnectNamedPipe(pipe.get());
      } else if (connected != IoResult::Stopped && !error.empty()) {
        record_error(error, false, false);
      }
    }
  }

  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  ControlChannelTransportSnapshot snapshot_;
  UniqueHandle stop_event_;
  std::thread worker_;
  std::wstring pipe_name_;
  std::wstring route_digest_;
  std::string engine_instance_id_;
  bool production_policy_{false};
  HANDLE active_pipe_{INVALID_HANDLE_VALUE};
  std::shared_ptr<CpuFrameMailboxProducer> active_mailbox_;
};

class SourceControlClient::Impl {
 public:
  bool start(std::wstring route, std::string& error) {
    // Lifecycle operations always acquire lifecycle_mutex_ before mutex_. The
    // worker never acquires lifecycle_mutex_, so stop can hold it through join.
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    const bool production_policy = uses_production_peer_policy(route);
    std::wstring pipe_name;
    if (!make_vividcam_control_pipe_name(route, pipe_name, error)) return false;
    std::wstring route_digest;
    if (!make_route_digest(route, route_digest, error)) return false;

    std::scoped_lock state_lock(mutex_);
    if (worker_.joinable()) {
      error = "Source control client is already running";
      return false;
    }
    if (!stop_event_.valid()) {
      stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!stop_event_.valid()) {
        error = windows_error("CreateEvent(client stop)", GetLastError());
        return false;
      }
    } else if (!ResetEvent(stop_event_.get())) {
      error = windows_error("ResetEvent(client stop)", GetLastError());
      return false;
    }

    pipe_name_ = std::move(pipe_name);
    route_digest_ = std::move(route_digest);
    production_policy_ = production_policy;
    snapshot_ = {};
    snapshot_.running = true;
    try {
      worker_ = std::thread([this] { run_guarded(); });
    } catch (const std::exception& exception) {
      snapshot_.running = false;
      snapshot_.last_error = exception.what();
      error = std::string("Could not start source control worker: ") +
              exception.what();
      return false;
    }
    error.clear();
    return true;
  }

  void stop() noexcept {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::thread worker;
    std::shared_ptr<CpuFrameMailboxSource> mailbox;
    {
      std::scoped_lock lock(mutex_);
      if (!worker_.joinable()) {
        snapshot_.running = false;
        snapshot_.connected = false;
        snapshot_.peer_process_id = 0;
        mailbox = std::move(active_mailbox_);
      } else {
        (void)SetEvent(stop_event_.get());
        if (active_pipe_ != INVALID_HANDLE_VALUE) {
          (void)CancelIoEx(active_pipe_, nullptr);
        }
        worker = std::move(worker_);
      }
    }
    if (mailbox) mailbox->close();
    if (!worker.joinable()) return;
    if (worker.joinable()) worker.join();
    {
      std::scoped_lock lock(mutex_);
      snapshot_.running = false;
      snapshot_.connected = false;
      snapshot_.peer_process_id = 0;
      active_pipe_ = INVALID_HANDLE_VALUE;
      mailbox = std::move(active_mailbox_);
    }
    if (mailbox) mailbox->close();
  }

  ControlChannelTransportSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
  }

  std::optional<CpuNv12Frame> take_latest_cpu_frame(std::string& error) {
    std::scoped_lock lock(mutex_);
    if (!active_mailbox_) {
      error.clear();
      return std::nullopt;
    }
    return active_mailbox_->take_latest(error);
  }

  CpuFrameMailboxSnapshot frame_mailbox_snapshot() const {
    std::scoped_lock lock(mutex_);
    return active_mailbox_ ? active_mailbox_->snapshot()
                           : CpuFrameMailboxSnapshot{};
  }

  std::wstring frame_mailbox_name() const {
    std::scoped_lock lock(mutex_);
    return active_mailbox_ ? active_mailbox_->name() : std::wstring{};
  }

 private:
  class PipePublicationGuard {
   public:
    PipePublicationGuard(Impl& owner, HANDLE pipe) noexcept
        : owner_(owner), pipe_(pipe) {}
    ~PipePublicationGuard() { unpublish(); }
    PipePublicationGuard(const PipePublicationGuard&) = delete;
    PipePublicationGuard& operator=(const PipePublicationGuard&) = delete;

    void unpublish() {
      if (pipe_ == INVALID_HANDLE_VALUE) return;
      owner_.clear_pipe(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }

   private:
    Impl& owner_;
    HANDLE pipe_;
  };

  class MailboxPublicationGuard {
   public:
    explicit MailboxPublicationGuard(Impl& owner) noexcept : owner_(owner) {}
    ~MailboxPublicationGuard() { close(); }
    MailboxPublicationGuard(const MailboxPublicationGuard&) = delete;
    MailboxPublicationGuard& operator=(const MailboxPublicationGuard&) = delete;

    void publish(std::shared_ptr<CpuFrameMailboxSource> mailbox) {
      mailbox_ = std::move(mailbox);
      resume();
    }

    void suspend() {
      if (!mailbox_ || !published_) return;
      owner_.clear_mailbox(mailbox_);
      published_ = false;
    }

    void resume() {
      if (!mailbox_ || published_) return;
      owner_.publish_mailbox(mailbox_);
      published_ = true;
    }

   private:
    void close() noexcept {
      if (!mailbox_) return;
      if (published_) owner_.clear_mailbox(mailbox_);
      mailbox_->close();
      mailbox_.reset();
      published_ = false;
    }

    Impl& owner_;
    std::shared_ptr<CpuFrameMailboxSource> mailbox_;
    bool published_{false};
  };

  void run_guarded() noexcept {
    try {
      worker_main();
    } catch (const std::exception& exception) {
      std::scoped_lock lock(mutex_);
      snapshot_.last_error =
          std::string("Source control worker stopped: ") + exception.what();
    } catch (...) {
      std::scoped_lock lock(mutex_);
      snapshot_.last_error = "Source control worker stopped unexpectedly";
    }
    std::shared_ptr<CpuFrameMailboxSource> mailbox;
    {
      std::scoped_lock lock(mutex_);
      snapshot_.running = false;
      snapshot_.connected = false;
      snapshot_.peer_process_id = 0;
      active_pipe_ = INVALID_HANDLE_VALUE;
      mailbox = std::move(active_mailbox_);
    }
    if (mailbox) mailbox->close();
  }

  void publish_pipe(HANDLE pipe) {
    std::scoped_lock lock(mutex_);
    active_pipe_ = pipe;
  }

  void clear_pipe(HANDLE pipe) {
    std::scoped_lock lock(mutex_);
    if (active_pipe_ == pipe) active_pipe_ = INVALID_HANDLE_VALUE;
    snapshot_.connected = false;
    snapshot_.peer_process_id = 0;
  }

  void publish_mailbox(const std::shared_ptr<CpuFrameMailboxSource>& mailbox) {
    std::scoped_lock lock(mutex_);
    active_mailbox_ = mailbox;
  }

  void clear_mailbox(const std::shared_ptr<CpuFrameMailboxSource>& mailbox) {
    std::scoped_lock lock(mutex_);
    if (active_mailbox_ == mailbox) active_mailbox_.reset();
  }

  void record_error(const std::string& error, bool protocol_error,
                    bool rejected_peer = false) {
    std::scoped_lock lock(mutex_);
    update_last_error(snapshot_, error);
    if (protocol_error) ++snapshot_.protocol_errors;
    if (rejected_peer) ++snapshot_.rejected_peers;
  }

  bool wait_for_retry(ControlChannelStateMachine& state) {
    while (!stop_requested(stop_event_.get())) {
      const auto deadline = state.next_retry_deadline();
      if (!deadline) return true;
      const auto now = Clock::now();
      if (now >= *deadline) {
        std::string state_error;
        const ControlChannelAdvanceResult advanced =
            state.advance(now, state_error);
        if (advanced == ControlChannelAdvanceResult::Rejected) {
          record_error(state_error, false);
          return false;
        }
        return true;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 *deadline - now) +
                             1ms;
      const auto bounded = std::min<std::int64_t>(
          remaining.count(), static_cast<std::int64_t>(INFINITE - 1));
      if (WaitForSingleObject(stop_event_.get(),
                              static_cast<DWORD>(bounded)) == WAIT_OBJECT_0) {
        return false;
      }
    }
    return false;
  }

  IoResult run_session(HANDLE pipe, ControlChannelStateMachine& state,
                       const std::wstring& verified_producer_user_sid,
                       std::string& session_error) {
    ConnectionId connection_id{};
    if (!random_connection_id(connection_id, session_error)) {
      return IoResult::Failed;
    }

    MessageHeader source_hello;
    source_hello.message_type = MessageType::SourceHello;
    source_hello.message_sequence = 1;
    source_hello.connection_id = connection_id;
    IoResult result = write_message(
        pipe, stop_event_.get(), source_hello,
        Clock::now() + kHandshakeTimeout, session_error);
    if (result != IoResult::Complete) return result;

    MessageHeader producer_hello;
    result = read_message(pipe, stop_event_.get(), producer_hello,
                          Clock::now() + kHandshakeTimeout, session_error);
    if (result != IoResult::Complete) return result;
    if (producer_hello.message_type != MessageType::ProducerHello ||
        producer_hello.message_sequence != 1 ||
        producer_hello.correlation_id != source_hello.message_sequence ||
        producer_hello.flags != 0 || producer_hello.payload_bytes != 0 ||
        !equal_connection_id(producer_hello.connection_id, connection_id)) {
      session_error = "ProducerHello handshake contract is invalid";
      return IoResult::ProtocolFailure;
    }

    const Clock::time_point negotiation_deadline =
        Clock::now() + kHandshakeTimeout;
    const producer_ipc::OpenStreamPayload open_stream =
        fixed_open_stream_payload();
    std::vector<std::byte> open_stream_bytes;
    if (!negotiation_payload_succeeded(
            producer_ipc::encode_open_stream_payload(open_stream,
                                                      open_stream_bytes),
            "Could not encode OpenStream", session_error)) {
      return IoResult::Failed;
    }
    MessageHeader open_stream_header;
    open_stream_header.message_type = MessageType::OpenStream;
    open_stream_header.message_sequence = 2;
    open_stream_header.correlation_id = producer_hello.message_sequence;
    open_stream_header.connection_id = connection_id;
    open_stream_header.payload_bytes =
        static_cast<std::uint32_t>(open_stream_bytes.size());
    result = write_message(pipe, stop_event_.get(), open_stream_header,
                           open_stream_bytes, negotiation_deadline,
                           session_error);
    if (result != IoResult::Complete) return result;

    std::vector<std::byte> payload;
    MessageHeader offer_header;
    result = read_message(pipe, stop_event_.get(), offer_header, payload,
                          negotiation_deadline, session_error);
    if (result != IoResult::Complete) return result;
    if (!valid_control_message(
            offer_header, MessageType::TransportOffer, connection_id,
            producer_hello.message_sequence, open_stream_header.message_sequence,
            producer_ipc::kTransportOfferPayloadBytes, session_error)) {
      return IoResult::ProtocolFailure;
    }
    producer_ipc::TransportOfferPayload offer;
    if (!negotiation_payload_succeeded(
            producer_ipc::decode_transport_offer_payload(payload, offer),
            "TransportOffer payload is invalid", session_error) ||
        !negotiation_payload_succeeded(
            producer_ipc::validate_transport_offer_for_open_stream(open_stream,
                                                                    offer),
            "TransportOffer does not match OpenStream", session_error)) {
      return IoResult::ProtocolFailure;
    }

    auto mailbox = create_cpu_frame_mailbox_source(
        mailbox_options(production_policy_, route_digest_, connection_id,
                        verified_producer_user_sid),
        session_error);
    if (!mailbox) return IoResult::Failed;

    const producer_ipc::TransportDescriptorPayload accepted_descriptor =
        transport_descriptor_for(offer);
    std::vector<std::byte> accepted_bytes;
    if (!negotiation_payload_succeeded(
            producer_ipc::encode_transport_descriptor_payload(
                accepted_descriptor, accepted_bytes),
            "Could not encode TransportAccepted", session_error)) {
      return IoResult::Failed;
    }
    MessageHeader accepted_header;
    accepted_header.message_type = MessageType::TransportAccepted;
    accepted_header.message_sequence = 3;
    accepted_header.correlation_id = offer_header.message_sequence;
    accepted_header.connection_id = connection_id;
    accepted_header.payload_bytes =
        static_cast<std::uint32_t>(accepted_bytes.size());
    result = write_message(pipe, stop_event_.get(), accepted_header,
                           accepted_bytes, negotiation_deadline,
                           session_error);
    if (result != IoResult::Complete) return result;

    MessageHeader ready_header;
    result = read_message(pipe, stop_event_.get(), ready_header, payload,
                          negotiation_deadline, session_error);
    if (result != IoResult::Complete) return result;
    if (!valid_control_message(
            ready_header, MessageType::StreamReady, connection_id,
            offer_header.message_sequence, accepted_header.message_sequence,
            producer_ipc::kTransportDescriptorPayloadBytes, session_error)) {
      return IoResult::ProtocolFailure;
    }
    producer_ipc::TransportDescriptorPayload ready_descriptor;
    if (!negotiation_payload_succeeded(
            producer_ipc::decode_transport_descriptor_payload(
                payload, ready_descriptor),
            "StreamReady payload is invalid", session_error) ||
        !negotiation_payload_succeeded(
            producer_ipc::validate_transport_descriptor_for_offer(
                offer, ready_descriptor),
            "StreamReady changed the accepted CPU mailbox offer",
            session_error)) {
      return IoResult::ProtocolFailure;
    }

    if (!state.mark_handshake_ready(Clock::now(), session_error)) {
      return IoResult::Failed;
    }
    MailboxPublicationGuard mailbox_publication(*this);
    mailbox_publication.publish(std::move(mailbox));
    {
      std::scoped_lock lock(mutex_);
      snapshot_.connected = true;
      ++snapshot_.successful_handshakes;
      snapshot_.last_error.clear();
    }

    std::uint64_t server_sequence = ready_header.message_sequence;
    std::uint64_t client_sequence = accepted_header.message_sequence;
    while (!stop_requested(stop_event_.get())) {
      MessageHeader heartbeat;
      result = read_message(pipe, stop_event_.get(), heartbeat,
                            Clock::now() + 250ms, session_error);
      if (result == IoResult::Timeout) {
        const ControlChannelAdvanceResult advanced =
            state.advance(Clock::now(), session_error);
        if (advanced == ControlChannelAdvanceResult::BecameStale) {
          mailbox_publication.suspend();
        }
        if (advanced == ControlChannelAdvanceResult::BecameReconnecting) {
          session_error = "producer heartbeat timed out";
          return IoResult::ReconnectScheduled;
        }
        if (advanced == ControlChannelAdvanceResult::Rejected) {
          return IoResult::Failed;
        }
        continue;
      }
      if (result != IoResult::Complete) return result;
      if (!valid_control_message(heartbeat, MessageType::Heartbeat,
                                 connection_id, server_sequence, 0,
                                 0, session_error)) {
        return IoResult::ProtocolFailure;
      }
      server_sequence = heartbeat.message_sequence;
      if (!state.receive_heartbeat(server_sequence, Clock::now(),
                                   session_error)) {
        return IoResult::ProtocolFailure;
      }
      if (production_policy_) {
        std::uint32_t reverified_process_id = 0;
        if (!verify_connected_server(pipe, true, reverified_process_id,
                                     nullptr, session_error)) {
          return IoResult::Failed;
        }
      }
      mailbox_publication.resume();

      ++client_sequence;
      MessageHeader acknowledgement;
      acknowledgement.message_type = MessageType::HeartbeatAck;
      acknowledgement.message_sequence = client_sequence;
      acknowledgement.correlation_id = server_sequence;
      acknowledgement.connection_id = connection_id;
      result = write_message(pipe, stop_event_.get(), acknowledgement,
                             Clock::now() + kHeartbeatAckTimeout,
                             session_error);
      if (result != IoResult::Complete) return result;
      std::scoped_lock lock(mutex_);
      ++snapshot_.heartbeat_acks;
    }
    return IoResult::Stopped;
  }

  void worker_main() {
    ControlChannelStateMachine state;
    while (!stop_requested(stop_event_.get())) {
      std::string error;
      if (!state.begin_connect(Clock::now(), error)) {
        record_error(error, false);
        break;
      }
      {
        std::scoped_lock lock(mutex_);
        ++snapshot_.connection_attempts;
      }

      const DWORD flags = FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                          SECURITY_IDENTIFICATION;
      UniqueHandle pipe(CreateFileW(
          pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
          OPEN_EXISTING, flags, nullptr));
      if (!pipe.valid()) {
        error = windows_error("CreateFile(control pipe)", GetLastError());
        std::string state_error;
        if (!state.mark_connection_failed(error, Clock::now(), state_error)) {
          record_error(state_error, false);
          break;
        }
        record_error(error, false);
        if (!wait_for_retry(state)) break;
        continue;
      }

      publish_pipe(pipe.get());
      // This guard is declared after `pipe`, so exception unwinding always
      // unpublishes before UniqueHandle closes the underlying object.
      PipePublicationGuard pipe_publication(*this, pipe.get());
      std::uint32_t server_process_id = 0;
      std::wstring verified_producer_user_sid;
      if (!verify_connected_server(pipe.get(), production_policy_,
                                   server_process_id,
                                   &verified_producer_user_sid, error)) {
        record_error(error, false, true);
        std::string state_error;
        if (!state.mark_connection_failed(error, Clock::now(), state_error)) {
          record_error(state_error, false);
          break;
        }
        pipe_publication.unpublish();
        pipe.reset();
        if (!wait_for_retry(state)) break;
        continue;
      }
      {
        std::scoped_lock lock(mutex_);
        snapshot_.peer_process_id = server_process_id;
      }

      if (!state.mark_transport_connected(Clock::now(), error)) {
        record_error(error, false);
        break;
      }
      std::string session_error;
      const IoResult result = run_session(pipe.get(), state,
                                          verified_producer_user_sid,
                                          session_error);
      pipe_publication.unpublish();
      pipe.reset();
      if (result == IoResult::Stopped || stop_requested(stop_event_.get())) {
        break;
      }
      record_error(session_error, result == IoResult::ProtocolFailure);

      if (result == IoResult::ReconnectScheduled) {
        if (!wait_for_retry(state)) break;
        continue;
      }

      std::string state_error;
      if (!state.mark_connection_failed(
              session_error.empty() ? "control session failed" : session_error,
              Clock::now(), state_error)) {
        record_error(state_error, false);
        break;
      }
      if (!wait_for_retry(state)) break;
    }
    std::string shutdown_error;
    if (!state.shutdown(Clock::now(), shutdown_error) &&
        !shutdown_error.empty()) {
      record_error(shutdown_error, false);
    }
  }

  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  ControlChannelTransportSnapshot snapshot_;
  UniqueHandle stop_event_;
  std::thread worker_;
  std::wstring pipe_name_;
  std::wstring route_digest_;
  bool production_policy_{false};
  HANDLE active_pipe_{INVALID_HANDLE_VALUE};
  std::shared_ptr<CpuFrameMailboxSource> active_mailbox_;
};

ProducerControlServer::ProducerControlServer() : impl_(std::make_unique<Impl>()) {}
ProducerControlServer::~ProducerControlServer() { stop(); }

bool ProducerControlServer::start(std::wstring route,
                                  std::string engine_instance_id,
                                  std::string& error) {
  return impl_->start(std::move(route), std::move(engine_instance_id), error);
}

void ProducerControlServer::stop() noexcept { impl_->stop(); }

ControlChannelTransportSnapshot ProducerControlServer::snapshot() const {
  return impl_->snapshot();
}

CpuFramePublishResult ProducerControlServer::publish_cpu_frame_for_mailbox(
    const CpuNv12Frame& frame, std::wstring_view expected_mailbox_name,
    std::string& error) {
  return impl_->publish_cpu_frame_for_mailbox(frame, expected_mailbox_name,
                                               error);
}

CpuFrameMailboxSnapshot ProducerControlServer::frame_mailbox_snapshot() const {
  return impl_->frame_mailbox_snapshot();
}

std::wstring ProducerControlServer::frame_mailbox_name() const {
  return impl_->frame_mailbox_name();
}

SourceControlClient::SourceControlClient() : impl_(std::make_unique<Impl>()) {}
SourceControlClient::~SourceControlClient() { stop(); }

bool SourceControlClient::start(std::wstring route, std::string& error) {
  return impl_->start(std::move(route), error);
}

void SourceControlClient::stop() noexcept { impl_->stop(); }

ControlChannelTransportSnapshot SourceControlClient::snapshot() const {
  return impl_->snapshot();
}

std::optional<CpuNv12Frame> SourceControlClient::take_latest_cpu_frame(
    std::string& error) {
  return impl_->take_latest_cpu_frame(error);
}

CpuFrameMailboxSnapshot SourceControlClient::frame_mailbox_snapshot() const {
  return impl_->frame_mailbox_snapshot();
}

std::wstring SourceControlClient::frame_mailbox_name() const {
  return impl_->frame_mailbox_name();
}

} // namespace vividcam
