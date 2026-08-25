#include "vividcam/control_channel_transport.hpp"

#include "vividcam/control_channel_state.hpp"
#include "vividcam/producer_ipc_protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
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
                      Clock::time_point deadline, std::string& error) {
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
    // The W4b-2a control messages are deliberately payload-free. Refuse the
    // payload before allocating or letting it desynchronize the byte stream.
    error = "W4b-2a control message payload must be empty";
    return IoResult::ProtocolFailure;
  }
  return IoResult::Complete;
}

IoResult write_message(HANDLE pipe, HANDLE stop_event,
                       const MessageHeader& header,
                       Clock::time_point deadline, std::string& error) {
  std::vector<std::byte> encoded;
  const producer_ipc::ProtocolError status =
      producer_ipc::encode_message(header, {}, encoded);
  if (status != producer_ipc::ProtocolError::None) {
    error = std::string(producer_ipc::protocol_error_message(status));
    return IoResult::ProtocolFailure;
  }
  return write_all(pipe, stop_event, encoded, deadline, error);
}

bool random_connection_id(ConnectionId& connection_id, std::string& error) {
  const NTSTATUS status = BCryptGenRandom(
      nullptr, connection_id.data(), static_cast<ULONG>(connection_id.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCryptGenRandom", status);
    return false;
  }
  error.clear();
  return true;
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
                           std::string& error) {
  if (header.message_type != type) {
    error = "unexpected control message type";
    return false;
  }
  if (header.flags != 0 || header.payload_bytes != 0) {
    error = "control message flags and payload must be zero";
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

bool build_pipe_security_descriptor(PSECURITY_DESCRIPTOR& descriptor,
                                    std::string& error) {
  descriptor = nullptr;
  TokenIdentity identity;
  if (!current_process_identity(identity, error)) return false;

  std::wstring logon_sid;
  if (!sid_to_string(identity.logon_sid, logon_sid, error)) {
    return false;
  }

  // The explicit protected DACL intentionally has no Everyone or Anonymous
  // access. The logon-SID ACE scopes normal clients to this sign-in session;
  // a broad user-SID ACE is deliberately omitted so another terminal session
  // using the same account cannot occupy the single handshake slot.
  const std::wstring sddl =
      L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;" + logon_sid + L")";
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

bool verify_connected_client(HANDLE pipe, std::uint32_t& process_id,
                             std::string& error) {
  process_id = 0;
  // Capture the server identity before impersonating. An identification-level
  // client token cannot be used to open the process token while impersonated.
  TokenIdentity current;
  if (!current_process_identity(current, error)) return false;
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

bool verify_connected_server(HANDLE pipe, std::uint32_t& process_id,
                             std::string& error) {
  process_id = 0;
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

  // This branch exists solely for the in-process loopback transport test. A
  // real FrameServer-hosted source and vividcam_engine.exe cannot share a PID.
  if (raw_process_id == GetCurrentProcessId()) {
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
  const std::wstring_view full_path(path.data(), path_length);
  const std::size_t separator = full_path.find_last_of(L"\\/");
  const std::wstring basename(
      separator == std::wstring_view::npos ? full_path
                                           : full_path.substr(separator + 1));
  if (_wcsicmp(basename.c_str(), L"vividcam_engine.exe") != 0) {
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

  // Image/token/session checks reduce accidental or low-effort pipe squatting,
  // but they are not nonce/signature-based cryptographic authentication.
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

} // namespace

bool make_vividcam_control_pipe_name(std::wstring_view route,
                                     std::wstring& pipe_name,
                                     std::string& error) {
  pipe_name.clear();
  std::array<std::uint8_t, 32> digest{};
  if (!hash_route(route, digest, error)) return false;

  constexpr wchar_t hex[] = L"0123456789abcdef";
  pipe_name.assign(kPipePrefix);
  pipe_name.reserve(pipe_name.size() + digest.size() * 2);
  for (std::uint8_t byte : digest) {
    pipe_name.push_back(hex[byte >> 4U]);
    pipe_name.push_back(hex[byte & 0x0fU]);
  }
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
  error.clear();
  return true;
}

class ProducerControlServer::Impl {
 public:
  bool start(std::wstring route, std::string engine_instance_id,
             std::string& error) {
    // Lifecycle operations always acquire lifecycle_mutex_ before mutex_. The
    // worker never acquires lifecycle_mutex_, so stop can hold it through join.
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::wstring pipe_name;
    if (!make_vividcam_control_pipe_name(route, pipe_name, error)) return false;
    if (engine_instance_id.empty()) {
      error = "Producer engine instance ID must not be empty";
      return false;
    }

    std::scoped_lock state_lock(mutex_);
    if (worker_.joinable()) {
      error = "Producer control server is already running";
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
    engine_instance_id_ = std::move(engine_instance_id);
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
    {
      std::scoped_lock lock(mutex_);
      if (!worker_.joinable()) {
        snapshot_.running = false;
        snapshot_.connected = false;
        snapshot_.peer_process_id = 0;
        return;
      }
      (void)SetEvent(stop_event_.get());
      // PipePublicationGuard clears this slot under the same mutex before the
      // owning UniqueHandle closes, so a non-invalid value cannot be stale or
      // refer to a subsequently reused kernel handle here.
      if (active_pipe_ != INVALID_HANDLE_VALUE) {
        (void)CancelIoEx(active_pipe_, nullptr);
      }
      worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
    std::scoped_lock lock(mutex_);
    snapshot_.running = false;
    snapshot_.connected = false;
    snapshot_.peer_process_id = 0;
    active_pipe_ = INVALID_HANDLE_VALUE;
  }

  ControlChannelTransportSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
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
    std::scoped_lock lock(mutex_);
    snapshot_.running = false;
    snapshot_.connected = false;
    snapshot_.peer_process_id = 0;
    // Defensive only: every published pipe is normally cleared by
    // PipePublicationGuard before its UniqueHandle closes.
    active_pipe_ = INVALID_HANDLE_VALUE;
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
    if (!verify_connected_client(pipe, client_process_id, session_error)) {
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

    MessageHeader producer_hello;
    producer_hello.message_type = MessageType::ProducerHello;
    producer_hello.message_sequence = 1;
    producer_hello.correlation_id = source_hello.message_sequence;
    producer_hello.connection_id = source_hello.connection_id;
    const IoResult write_hello = write_message(
        pipe, stop_event_.get(), producer_hello,
        Clock::now() + kHandshakeTimeout, session_error);
    if (write_hello != IoResult::Complete) {
      if (write_hello == IoResult::ProtocolFailure) {
        record_error(session_error, true, false);
      }
      return false;
    }

    {
      std::scoped_lock lock(mutex_);
      snapshot_.connected = true;
      snapshot_.peer_process_id = client_process_id;
      ++snapshot_.successful_handshakes;
      snapshot_.last_error.clear();
    }

    std::uint64_t server_sequence = 1;
    std::uint64_t client_sequence = source_hello.message_sequence;
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
              session_error)) {
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
    // The engine instance ID is intentionally not serialized in W4b-2a: all
    // four handshake/heartbeat messages have zero-length payloads.
    (void)engine_instance_id_;
    while (!stop_requested(stop_event_.get())) {
      PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
      std::string error;
      if (!build_pipe_security_descriptor(raw_descriptor, error)) {
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
  std::string engine_instance_id_;
  HANDLE active_pipe_{INVALID_HANDLE_VALUE};
};

class SourceControlClient::Impl {
 public:
  bool start(std::wstring route, std::string& error) {
    // Lifecycle operations always acquire lifecycle_mutex_ before mutex_. The
    // worker never acquires lifecycle_mutex_, so stop can hold it through join.
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::wstring pipe_name;
    if (!make_vividcam_control_pipe_name(route, pipe_name, error)) return false;

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
    {
      std::scoped_lock lock(mutex_);
      if (!worker_.joinable()) {
        snapshot_.running = false;
        snapshot_.connected = false;
        snapshot_.peer_process_id = 0;
        return;
      }
      (void)SetEvent(stop_event_.get());
      // PipePublicationGuard clears this slot under the same mutex before the
      // owning UniqueHandle closes, so a non-invalid value cannot be stale or
      // refer to a subsequently reused kernel handle here.
      if (active_pipe_ != INVALID_HANDLE_VALUE) {
        (void)CancelIoEx(active_pipe_, nullptr);
      }
      worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
    std::scoped_lock lock(mutex_);
    snapshot_.running = false;
    snapshot_.connected = false;
    snapshot_.peer_process_id = 0;
    active_pipe_ = INVALID_HANDLE_VALUE;
  }

  ControlChannelTransportSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
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
    std::scoped_lock lock(mutex_);
    snapshot_.running = false;
    snapshot_.connected = false;
    snapshot_.peer_process_id = 0;
    // Defensive only: every published pipe is normally cleared by
    // PipePublicationGuard before its UniqueHandle closes.
    active_pipe_ = INVALID_HANDLE_VALUE;
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

    if (!state.mark_handshake_ready(Clock::now(), session_error)) {
      return IoResult::Failed;
    }
    {
      std::scoped_lock lock(mutex_);
      snapshot_.connected = true;
      ++snapshot_.successful_handshakes;
      snapshot_.last_error.clear();
    }

    std::uint64_t server_sequence = producer_hello.message_sequence;
    std::uint64_t client_sequence = source_hello.message_sequence;
    while (!stop_requested(stop_event_.get())) {
      MessageHeader heartbeat;
      result = read_message(pipe, stop_event_.get(), heartbeat,
                            Clock::now() + 250ms, session_error);
      if (result == IoResult::Timeout) {
        const ControlChannelAdvanceResult advanced =
            state.advance(Clock::now(), session_error);
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
                                 session_error)) {
        return IoResult::ProtocolFailure;
      }
      server_sequence = heartbeat.message_sequence;
      if (!state.receive_heartbeat(server_sequence, Clock::now(),
                                   session_error)) {
        return IoResult::ProtocolFailure;
      }

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
      if (!verify_connected_server(pipe.get(), server_process_id, error)) {
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
      const IoResult result = run_session(pipe.get(), state, session_error);
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
  HANDLE active_pipe_{INVALID_HANDLE_VALUE};
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

SourceControlClient::SourceControlClient() : impl_(std::make_unique<Impl>()) {}
SourceControlClient::~SourceControlClient() { stop(); }

bool SourceControlClient::start(std::wstring route, std::string& error) {
  return impl_->start(std::move(route), error);
}

void SourceControlClient::stop() noexcept { impl_->stop(); }

ControlChannelTransportSnapshot SourceControlClient::snapshot() const {
  return impl_->snapshot();
}

} // namespace vividcam
