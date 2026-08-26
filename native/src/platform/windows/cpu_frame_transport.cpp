#include "vividcam/cpu_frame_transport.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace vividcam {
namespace {

using namespace cpu_frame_mailbox_layout;

static_assert(kMappingBytes < std::numeric_limits<std::uint32_t>::max());
static_assert(kPublishedGenerationOffset % alignof(LONG64) == 0);
static_assert(kConsumedGenerationOffset % alignof(LONG64) == 0);
static_assert(kProducerClaimOffset % alignof(LONG64) == 0);
static_assert(kSlotStrideBytes % alignof(LONG64) == 0);

bool uses_production_security(CpuFrameMailboxScope scope) noexcept {
  return scope == CpuFrameMailboxScope::ProductionGlobal ||
         scope == CpuFrameMailboxScope::ProductionSecurityLocalTest;
}

class LocalAllocation {
 public:
  explicit LocalAllocation(void* value = nullptr) noexcept : value_(value) {}
  ~LocalAllocation() {
    if (value_) LocalFree(value_);
  }
  LocalAllocation(const LocalAllocation&) = delete;
  LocalAllocation& operator=(const LocalAllocation&) = delete;
  LocalAllocation(LocalAllocation&& other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  LocalAllocation& operator=(LocalAllocation&& other) noexcept {
    if (this == &other) return *this;
    if (value_) LocalFree(value_);
    value_ = std::exchange(other.value_, nullptr);
    return *this;
  }
  [[nodiscard]] void* get() const noexcept { return value_; }

 private:
  void* value_{nullptr};
};

std::string windows_error(const char* operation, DWORD status) {
  char* raw_message = nullptr;
  DWORD characters = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, status, 0, reinterpret_cast<char*>(&raw_message), 0, nullptr);
  LocalAllocation message_owner(raw_message);
  std::string message(operation);
  message += " failed (win32=" + std::to_string(status) + ')';
  if (characters != 0 && raw_message) {
    while (characters > 0 &&
           (raw_message[characters - 1U] == '\r' ||
            raw_message[characters - 1U] == '\n')) {
      raw_message[characters - 1U] = '\0';
      // FormatMessage owns the buffer; trimming its tail is safe.
      --characters;
    }
    if (raw_message[0] != '\0') {
      message += ": ";
      message += raw_message;
    }
  }
  return message;
}

void write_u16(std::byte* destination, std::size_t offset,
               std::uint16_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void write_u32(std::byte* destination, std::size_t offset,
               std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void write_u64(std::byte* destination, std::size_t offset,
               std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

std::uint16_t read_u16(const std::byte* source, std::size_t offset) noexcept {
  std::uint16_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint16_t>(
                 std::to_integer<std::uint8_t>(source[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::uint32_t read_u32(const std::byte* source, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(source[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_u64(const std::byte* source, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(source[offset + index]))
             << (index * 8U);
  }
  return value;
}

volatile LONG64* atomic_u64(std::byte* base, std::size_t offset) noexcept {
  return reinterpret_cast<volatile LONG64*>(base + offset);
}

const volatile LONG64* atomic_u64(const std::byte* base,
                                  std::size_t offset) noexcept {
  return reinterpret_cast<const volatile LONG64*>(base + offset);
}

std::uint64_t load_atomic(const std::byte* base, std::size_t offset) noexcept {
  auto* field = const_cast<volatile LONG64*>(atomic_u64(base, offset));
  return static_cast<std::uint64_t>(InterlockedCompareExchange64(field, 0, 0));
}

void store_atomic(std::byte* base, std::size_t offset,
                  std::uint64_t value) noexcept {
  (void)InterlockedExchange64(atomic_u64(base, offset),
                              static_cast<LONG64>(value));
}

void increment_atomic(std::byte* base, std::size_t offset) noexcept {
  (void)InterlockedIncrement64(atomic_u64(base, offset));
}

bool all_zero(const std::byte* bytes, std::size_t begin,
              std::size_t end) noexcept {
  for (std::size_t index = begin; index < end; ++index) {
    if (bytes[index] != std::byte{0}) return false;
  }
  return true;
}

bool canonical_sid_string(std::wstring_view input, std::wstring& canonical,
                          std::string& error) {
  canonical.clear();
  if (input.empty() || input.find(L'\0') != std::wstring_view::npos) {
    error = "Production CPU frame mailbox producer SID is empty or malformed";
    return false;
  }
  PSID raw_sid = nullptr;
  const std::wstring owned_input(input);
  if (!ConvertStringSidToSidW(owned_input.c_str(), &raw_sid)) {
    error = windows_error("ConvertStringSidToSid(producer)", GetLastError());
    return false;
  }
  LocalAllocation sid_owner(raw_sid);
  if (!IsValidSid(raw_sid)) {
    error = "Production CPU frame mailbox producer SID is invalid";
    return false;
  }
  wchar_t* raw_string = nullptr;
  if (!ConvertSidToStringSidW(raw_sid, &raw_string)) {
    error = windows_error("ConvertSidToStringSid(producer)", GetLastError());
    return false;
  }
  LocalAllocation string_owner(raw_string);
  canonical.assign(raw_string);
  error.clear();
  return true;
}

bool frame_server_sid_string(std::wstring& sid_string, std::string& error) {
  constexpr wchar_t account[] = L"NT SERVICE\\FrameServer";
  DWORD sid_bytes = 0;
  DWORD domain_characters = 0;
  SID_NAME_USE sid_type = SidTypeUnknown;
  (void)LookupAccountNameW(nullptr, account, nullptr, &sid_bytes, nullptr,
                           &domain_characters, &sid_type);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sid_bytes == 0) {
    error = windows_error("LookupAccountName(FrameServer size)", GetLastError());
    return false;
  }
  std::vector<std::byte> sid(sid_bytes);
  std::vector<wchar_t> domain(std::max<DWORD>(domain_characters, 1U), L'\0');
  DWORD sid_capacity = sid_bytes;
  DWORD domain_capacity = static_cast<DWORD>(domain.size());
  if (!LookupAccountNameW(nullptr, account, sid.data(), &sid_capacity,
                          domain.data(), &domain_capacity, &sid_type) ||
      !IsValidSid(sid.data())) {
    error = windows_error("LookupAccountName(FrameServer)", GetLastError());
    return false;
  }
  wchar_t* raw_string = nullptr;
  if (!ConvertSidToStringSidW(sid.data(), &raw_string)) {
    error = windows_error("ConvertSidToStringSid(FrameServer)", GetLastError());
    return false;
  }
  LocalAllocation string_owner(raw_string);
  sid_string.assign(raw_string);
  error.clear();
  return true;
}

bool parse_sid_bytes(std::wstring_view value, std::vector<std::byte>& sid,
                     std::string& error) {
  sid.clear();
  std::wstring canonical;
  if (!canonical_sid_string(value, canonical, error)) return false;
  PSID raw_sid = nullptr;
  if (!ConvertStringSidToSidW(canonical.c_str(), &raw_sid)) {
    error = windows_error("ConvertStringSidToSid(expected producer)",
                          GetLastError());
    return false;
  }
  LocalAllocation sid_owner(raw_sid);
  const DWORD bytes = GetLengthSid(raw_sid);
  if (bytes == 0) {
    error = "Expected producer SID has zero length";
    return false;
  }
  sid.resize(bytes);
  if (!CopySid(bytes, sid.data(), raw_sid)) {
    error = windows_error("CopySid(expected producer)", GetLastError());
    sid.clear();
    return false;
  }
  error.clear();
  return true;
}

bool frame_server_sid_bytes(std::vector<std::byte>& sid, std::string& error) {
  std::wstring value;
  return frame_server_sid_string(value, error) &&
         parse_sid_bytes(value, sid, error);
}

bool system_sid_bytes(std::vector<std::byte>& sid, std::string& error) {
  sid.assign(SECURITY_MAX_SID_SIZE, std::byte{0});
  DWORD bytes = static_cast<DWORD>(sid.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, sid.data(), &bytes)) {
    error = windows_error("CreateWellKnownSid(LocalSystem)", GetLastError());
    sid.clear();
    return false;
  }
  sid.resize(bytes);
  error.clear();
  return true;
}

bool build_production_security_descriptor(
    std::wstring_view producer_user_sid, PSECURITY_DESCRIPTOR& descriptor,
    std::string& error) {
  descriptor = nullptr;
  std::wstring producer_sid;
  std::wstring frame_server_sid;
  if (!canonical_sid_string(producer_user_sid, producer_sid, error) ||
      !frame_server_sid_string(frame_server_sid, error)) {
    return false;
  }

  // The protected DACL excludes broad LocalService, interactive, Users, and
  // Everyone principals. The explicit Medium mandatory label is essential:
  // an object created by the session-0 service would otherwise inherit a high
  // integrity label and deny writes by the intended medium-integrity engine.
  // Do not mark the label SACL protected: creating a protected SACL requires
  // SeSecurityPrivilege, which the LocalService FrameServer does not have.
  // Exact post-create label validation remains fail-closed if inheritance ever
  // adds or changes an ACE.
  const std::wstring sddl =
      L"D:P(A;;GA;;;SY)(A;;GA;;;" + frame_server_sid +
      L")(A;;GRGW;;;" + producer_sid + L")S:(ML;;NW;;;ME)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    error = windows_error("ConvertStringSecurityDescriptor(frame mailbox)",
                          GetLastError());
    return false;
  }
  error.clear();
  return true;
}

bool exact_mapping_mask(ACCESS_MASK actual, ACCESS_MASK generic,
                        ACCESS_MASK mapped) noexcept {
  // Depending on where the descriptor is observed, the object manager may
  // retain the generic SDDL bits or map them to SECTION_* rights. No other bit
  // combination is accepted.
  return actual == generic || actual == mapped;
}

bool verify_production_mapping_security(HANDLE mapping,
                                        std::wstring_view producer_user_sid,
                                        std::string& error) {
  std::vector<std::byte> system_sid;
  std::vector<std::byte> frame_server_sid;
  std::vector<std::byte> producer_sid;
  if (!system_sid_bytes(system_sid, error) ||
      !frame_server_sid_bytes(frame_server_sid, error) ||
      !parse_sid_bytes(producer_user_sid, producer_sid, error)) {
    return false;
  }

  PACL dacl = nullptr;
  PACL sacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD status = GetSecurityInfo(
      mapping, SE_KERNEL_OBJECT,
      DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, nullptr, nullptr,
      &dacl, &sacl, &descriptor);
  if (status != ERROR_SUCCESS) {
    error = windows_error("GetSecurityInfo(CPU frame mailbox)", status);
    return false;
  }
  LocalAllocation descriptor_owner(descriptor);
  if (!dacl || !IsValidAcl(dacl) || !sacl || !IsValidAcl(sacl)) {
    error = "CPU frame mailbox DACL or mandatory label ACL is invalid";
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      (control & SE_DACL_PROTECTED) == 0) {
    error = "CPU frame mailbox DACL is not protected";
    return false;
  }

  constexpr ACCESS_MASK kMappedAll = SECTION_ALL_ACCESS;
  constexpr ACCESS_MASK kMappedProducer =
      READ_CONTROL | SECTION_QUERY | SECTION_MAP_READ | SECTION_MAP_WRITE;
  bool saw_system = false;
  bool saw_frame_server = false;
  bool saw_producer = false;
  if (dacl->AceCount != 3) {
    error = "CPU frame mailbox DACL must contain exactly three allow ACEs";
    return false;
  }
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (!GetAce(dacl, index, &raw_ace)) {
      error = windows_error("GetAce(CPU frame mailbox DACL)", GetLastError());
      return false;
    }
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags != 0) {
      error = "CPU frame mailbox DACL contains a non-direct allow ACE";
      return false;
    }
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    PSID sid = const_cast<DWORD*>(&ace->SidStart);
    if (!IsValidSid(sid)) {
      error = "CPU frame mailbox DACL contains an invalid SID";
      return false;
    }
    if (EqualSid(sid, system_sid.data())) {
      if (saw_system ||
          !exact_mapping_mask(ace->Mask, GENERIC_ALL, kMappedAll)) {
        error = "CPU frame mailbox SYSTEM ACE is duplicated or overbroad";
        return false;
      }
      saw_system = true;
    } else if (EqualSid(sid, frame_server_sid.data())) {
      if (saw_frame_server ||
          !exact_mapping_mask(ace->Mask, GENERIC_ALL, kMappedAll)) {
        error = "CPU frame mailbox FrameServer ACE is duplicated or overbroad";
        return false;
      }
      saw_frame_server = true;
    } else if (EqualSid(sid, producer_sid.data())) {
      if (saw_producer ||
          !exact_mapping_mask(ace->Mask, GENERIC_READ | GENERIC_WRITE,
                              kMappedProducer)) {
        error = "CPU frame mailbox producer ACE is duplicated or overbroad";
        return false;
      }
      saw_producer = true;
    } else {
      error = "CPU frame mailbox DACL contains an unexpected principal";
      return false;
    }
  }
  if (!saw_system || !saw_frame_server || !saw_producer) {
    error = "CPU frame mailbox DACL is missing an exact required principal";
    return false;
  }

  if (sacl->AceCount != 1) {
    error = "CPU frame mailbox must have exactly one mandatory label ACE";
    return false;
  }
  void* raw_label = nullptr;
  if (!GetAce(sacl, 0, &raw_label)) {
    error = windows_error("GetAce(CPU frame mailbox label)", GetLastError());
    return false;
  }
  const auto* label_header = static_cast<const ACE_HEADER*>(raw_label);
  if (label_header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE ||
      label_header->AceFlags != 0) {
    error = "CPU frame mailbox mandatory label ACE type or flags are invalid";
    return false;
  }
  const auto* label = static_cast<const SYSTEM_MANDATORY_LABEL_ACE*>(raw_label);
  PSID label_sid = const_cast<DWORD*>(&label->SidStart);
  if (label->Mask != SYSTEM_MANDATORY_LABEL_NO_WRITE_UP ||
      !IsValidSid(label_sid) ||
      !IsWellKnownSid(label_sid, WinMediumLabelSid)) {
    error = "CPU frame mailbox mandatory label is not exact Medium/no-write-up";
    return false;
  }
  error.clear();
  return true;
}

class MappingLease {
 public:
  MappingLease(HANDLE mapping, void* view, std::wstring name) noexcept
      : mapping_(mapping), view_(static_cast<std::byte*>(view)),
        name_(std::move(name)) {}
  ~MappingLease() {
    release_writer_claim();
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
  }
  MappingLease(const MappingLease&) = delete;
  MappingLease& operator=(const MappingLease&) = delete;

  [[nodiscard]] std::byte* bytes() noexcept { return view_; }
  [[nodiscard]] const std::byte* bytes() const noexcept { return view_; }
  [[nodiscard]] const std::wstring& name() const noexcept { return name_; }
  [[nodiscard]] HANDLE handle() const noexcept { return mapping_; }
  [[nodiscard]] bool claim_writer() noexcept {
    if (!view_ || writer_claimed_) return false;
    if (InterlockedCompareExchange64(atomic_u64(view_, kProducerClaimOffset),
                                     1, 0) != 0) {
      return false;
    }
    writer_claimed_ = true;
    return true;
  }

 private:
  void release_writer_claim() noexcept {
    if (!view_ || !writer_claimed_) return;
    (void)InterlockedCompareExchange64(
        atomic_u64(view_, kProducerClaimOffset), 0, 1);
    writer_claimed_ = false;
  }

  HANDLE mapping_{nullptr};
  std::byte* view_{nullptr};
  std::wstring name_;
  bool writer_claimed_{false};
};

void initialize_mapping(MappingLease& mapping,
                        const CpuFrameConnectionId& connection_id) noexcept {
  std::byte* bytes = mapping.bytes();
  std::memset(bytes, 0, kMappingBytes);
  std::copy(kMagic.begin(), kMagic.end(),
            reinterpret_cast<std::uint8_t*>(bytes + kMagicOffset));
  write_u16(bytes, kLayoutVersionOffset,
            static_cast<std::uint16_t>(kCpuFrameMailboxLayoutVersion));
  write_u16(bytes, kHeaderBytesOffset,
            static_cast<std::uint16_t>(kHeaderBytes));
  write_u64(bytes, kMappingBytesOffset, kMappingBytes);
  write_u32(bytes, kWidthOffset, kCpuFrameWidth);
  write_u32(bytes, kHeightOffset, kCpuFrameHeight);
  write_u32(bytes, kPixelFormatOffset, kPixelFormatNv12);
  write_u32(bytes, kSlotCountOffset,
            static_cast<std::uint32_t>(kCpuFrameMailboxSlotCount));
  write_u32(bytes, kYStrideOffset, kCpuFrameYStrideBytes);
  write_u32(bytes, kUvStrideOffset, kCpuFrameUvStrideBytes);
  write_u64(bytes, kFrameBytesOffset, kCpuFrameNv12Bytes);
  std::copy(connection_id.begin(), connection_id.end(),
            reinterpret_cast<std::uint8_t*>(bytes + kConnectionIdOffset));
  MemoryBarrier();
}

bool valid_mapping_header(const MappingLease& mapping,
                          const CpuFrameConnectionId& connection_id) noexcept {
  const std::byte* bytes = mapping.bytes();
  if (!std::equal(kMagic.begin(), kMagic.end(),
                  reinterpret_cast<const std::uint8_t*>(bytes + kMagicOffset)) ||
      read_u16(bytes, kLayoutVersionOffset) !=
          kCpuFrameMailboxLayoutVersion ||
      read_u16(bytes, kHeaderBytesOffset) != kHeaderBytes ||
      read_u64(bytes, kMappingBytesOffset) != kMappingBytes ||
      read_u32(bytes, kWidthOffset) != kCpuFrameWidth ||
      read_u32(bytes, kHeightOffset) != kCpuFrameHeight ||
      read_u32(bytes, kPixelFormatOffset) != kPixelFormatNv12 ||
      read_u32(bytes, kSlotCountOffset) != kCpuFrameMailboxSlotCount ||
      read_u32(bytes, kYStrideOffset) != kCpuFrameYStrideBytes ||
      read_u32(bytes, kUvStrideOffset) != kCpuFrameUvStrideBytes ||
      read_u64(bytes, kFrameBytesOffset) != kCpuFrameNv12Bytes ||
      !std::equal(connection_id.begin(), connection_id.end(),
                  reinterpret_cast<const std::uint8_t*>(
                      bytes + kConnectionIdOffset)) ||
      !all_zero(bytes, 64, kPublishedGenerationOffset) ||
      load_atomic(bytes, kProducerClaimOffset) > 1U ||
      !all_zero(bytes, kProducerClaimOffset + sizeof(std::uint64_t),
                 kHeaderBytes)) {
    return false;
  }
  return true;
}

std::shared_ptr<MappingLease> create_mapping(
    const CpuFrameMailboxOptions& options, std::string& error) {
  std::wstring name;
  if (!make_cpu_frame_mailbox_name(options, name, error)) return nullptr;

  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  LocalAllocation descriptor_owner;
  SECURITY_ATTRIBUTES security{};
  SECURITY_ATTRIBUTES* security_pointer = nullptr;
  if (uses_production_security(options.scope)) {
    if (!build_production_security_descriptor(
            options.producer_user_sid, raw_descriptor, error)) {
      return nullptr;
    }
    descriptor_owner = LocalAllocation(raw_descriptor);
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = raw_descriptor;
    security.bInheritHandle = FALSE;
    security_pointer = &security;
  }

  const std::uint64_t mapping_bytes = kMappingBytes;
  HANDLE handle = CreateFileMappingW(
      INVALID_HANDLE_VALUE, security_pointer, PAGE_READWRITE,
      static_cast<DWORD>(mapping_bytes >> 32U),
      static_cast<DWORD>(mapping_bytes & 0xffffffffULL), name.c_str());
  if (!handle) {
    error = windows_error("CreateFileMapping(CPU frame mailbox)", GetLastError());
    return nullptr;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(handle);
    error = "CPU frame mailbox name already exists";
    return nullptr;
  }
  if (uses_production_security(options.scope) &&
      !verify_production_mapping_security(handle, options.producer_user_sid,
                                           error)) {
    CloseHandle(handle);
    return nullptr;
  }
  void* view = MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                             kMappingBytes);
  if (!view) {
    const DWORD status = GetLastError();
    CloseHandle(handle);
    error = windows_error("MapViewOfFile(CPU frame mailbox source)", status);
    return nullptr;
  }
  auto mapping = std::make_shared<MappingLease>(handle, view, std::move(name));
  initialize_mapping(*mapping, options.connection_id);
  error.clear();
  return mapping;
}

std::shared_ptr<MappingLease> open_mapping(
    const CpuFrameMailboxOptions& options, std::string& error) {
  std::wstring name;
  if (!make_cpu_frame_mailbox_name(options, name, error)) return nullptr;
  const DWORD desired_access =
      FILE_MAP_READ | FILE_MAP_WRITE |
      (uses_production_security(options.scope) ? READ_CONTROL : 0U);
  HANDLE handle = OpenFileMappingW(desired_access, FALSE, name.c_str());
  if (!handle) {
    error = windows_error("OpenFileMapping(CPU frame mailbox)", GetLastError());
    return nullptr;
  }
  if (uses_production_security(options.scope) &&
      !verify_production_mapping_security(handle, options.producer_user_sid,
                                           error)) {
    CloseHandle(handle);
    return nullptr;
  }
  void* view = MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                             kMappingBytes);
  if (!view) {
    const DWORD status = GetLastError();
    CloseHandle(handle);
    error = windows_error("MapViewOfFile(CPU frame mailbox producer)", status);
    return nullptr;
  }
  auto mapping = std::make_shared<MappingLease>(handle, view, std::move(name));
  if (!valid_mapping_header(*mapping, options.connection_id)) {
    error = "CPU frame mailbox header does not match the negotiated layout";
    return nullptr;
  }
  if (!mapping->claim_writer()) {
    error = "CPU frame mailbox already has a producer writer";
    return nullptr;
  }
  error.clear();
  return mapping;
}

CpuFrameMailboxSnapshot mapping_snapshot(
    const std::shared_ptr<MappingLease>& mapping) noexcept {
  if (!mapping) return {};
  const std::byte* bytes = mapping->bytes();
  return {true,
          load_atomic(bytes, kPublishedGenerationOffset),
          load_atomic(bytes, kConsumedGenerationOffset),
          load_atomic(bytes, kPublishedFramesOffset),
          load_atomic(bytes, kConsumedFramesOffset),
          load_atomic(bytes, kOverwrittenFramesOffset),
          load_atomic(bytes, kTornReadsOffset),
          load_atomic(bytes, kInvalidFramesOffset)};
}

} // namespace

class CpuFrameMailboxSource::Impl {
 public:
  explicit Impl(std::shared_ptr<MappingLease> mapping,
                CpuFrameConnectionId connection_id)
      : mapping_(std::move(mapping)), connection_id_(connection_id) {}

  std::optional<CpuNv12Frame> take_latest(std::string& error) {
    std::scoped_lock take_lock(take_mutex_);
    const auto mapping = mapping_;
    std::byte* bytes = mapping->bytes();
    if (!valid_mapping_header(*mapping, connection_id_)) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox header became invalid";
      return std::nullopt;
    }

    const std::uint64_t generation =
        load_atomic(bytes, kPublishedGenerationOffset);
    if (generation == 0 || generation == last_taken_generation_) {
      error.clear();
      return std::nullopt;
    }
    if (generation < last_taken_generation_ ||
        generation > static_cast<std::uint64_t>(
                         std::numeric_limits<LONG64>::max())) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox published generation is invalid";
      return std::nullopt;
    }

    const std::size_t slot_index =
        static_cast<std::size_t>((generation - 1U) % kCpuFrameMailboxSlotCount);
    const std::size_t slot = slot_offset(slot_index);
    const std::uint64_t begin =
        load_atomic(bytes, slot + kSlotBeginGenerationOffset);
    if (begin != generation) {
      increment_atomic(bytes, kTornReadsOffset);
      error.clear();
      return std::nullopt;
    }

    const std::uint64_t producer_sequence =
        read_u64(bytes, slot + kSlotProducerSequenceOffset);
    const std::uint64_t raw_timestamp =
        read_u64(bytes, slot + kSlotTimestampOffset);
    const std::uint64_t payload_bytes =
        read_u64(bytes, slot + kSlotPayloadBytesOffset);
    if (producer_sequence == 0 ||
        raw_timestamp >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        payload_bytes != kCpuFrameNv12Bytes) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox slot metadata is invalid";
      return std::nullopt;
    }

    CpuNv12Frame frame;
    try {
      frame.bytes.resize(kCpuFrameNv12Bytes);
    } catch (const std::bad_alloc&) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "Unable to allocate a CPU frame mailbox snapshot";
      return std::nullopt;
    }
    frame.sequence = producer_sequence;
    frame.timestamp_100ns = static_cast<std::int64_t>(raw_timestamp);
    std::memcpy(frame.bytes.data(),
                bytes + slot + kSlotPayloadOffset, kCpuFrameNv12Bytes);
    MemoryBarrier();
    const std::uint64_t end =
        load_atomic(bytes, slot + kSlotEndGenerationOffset);
    const std::uint64_t begin_after =
        load_atomic(bytes, slot + kSlotBeginGenerationOffset);
    // A newer generation may legitimately be published into the other slot
    // during this copy. Only changes to the slot being copied make it torn.
    if (begin_after != generation || end != generation) {
      increment_atomic(bytes, kTornReadsOffset);
      error.clear();
      return std::nullopt;
    }
    if (!frame.valid()) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox produced an invalid NV12 frame";
      return std::nullopt;
    }

    last_taken_generation_ = generation;
    store_atomic(bytes, kConsumedGenerationOffset, generation);
    increment_atomic(bytes, kConsumedFramesOffset);
    error.clear();
    return frame;
  }

  [[nodiscard]] CpuFrameMailboxSnapshot snapshot() const noexcept {
    return mapping_snapshot(mapping_);
  }
  [[nodiscard]] std::wstring name() const { return mapping_->name(); }

 private:
  std::shared_ptr<MappingLease> mapping_;
  CpuFrameConnectionId connection_id_{};
  std::mutex take_mutex_;
  std::uint64_t last_taken_generation_{0};
};

class CpuFrameMailboxProducer::Impl {
 public:
  explicit Impl(std::shared_ptr<MappingLease> mapping,
                CpuFrameConnectionId connection_id)
      : mapping_(std::move(mapping)), connection_id_(connection_id) {}

  bool publish(const CpuNv12Frame& frame, std::string& error) {
    std::scoped_lock publish_lock(publish_mutex_);
    std::byte* bytes = mapping_->bytes();
    if (!valid_mapping_header(*mapping_, connection_id_)) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox header became invalid";
      return false;
    }
    if (load_atomic(bytes, kProducerClaimOffset) != 1U) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox producer writer claim was lost";
      return false;
    }
    if (!frame.valid()) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox accepts packed 1920x1080 NV12 frames only";
      return false;
    }

    const std::uint64_t previous =
        load_atomic(bytes, kPublishedGenerationOffset);
    if (previous >= static_cast<std::uint64_t>(
                        std::numeric_limits<LONG64>::max())) {
      increment_atomic(bytes, kInvalidFramesOffset);
      error = "CPU frame mailbox generation is exhausted";
      return false;
    }
    const std::uint64_t consumed =
        load_atomic(bytes, kConsumedGenerationOffset);
    if (previous != 0 && previous > consumed) {
      increment_atomic(bytes, kOverwrittenFramesOffset);
    }

    const std::uint64_t generation = previous + 1U;
    const std::size_t slot_index =
        static_cast<std::size_t>((generation - 1U) % kCpuFrameMailboxSlotCount);
    const std::size_t slot = slot_offset(slot_index);
    store_atomic(bytes, slot + kSlotBeginGenerationOffset, 0);
    store_atomic(bytes, slot + kSlotEndGenerationOffset, 0);
    MemoryBarrier();
    write_u64(bytes, slot + kSlotProducerSequenceOffset, frame.sequence);
    write_u64(bytes, slot + kSlotTimestampOffset,
              static_cast<std::uint64_t>(frame.timestamp_100ns));
    write_u64(bytes, slot + kSlotPayloadBytesOffset, kCpuFrameNv12Bytes);
    std::memcpy(bytes + slot + kSlotPayloadOffset, frame.bytes.data(),
                kCpuFrameNv12Bytes);
    MemoryBarrier();
    store_atomic(bytes, slot + kSlotEndGenerationOffset, generation);
    store_atomic(bytes, slot + kSlotBeginGenerationOffset, generation);
    increment_atomic(bytes, kPublishedFramesOffset);
    store_atomic(bytes, kPublishedGenerationOffset, generation);
    error.clear();
    return true;
  }

  [[nodiscard]] CpuFrameMailboxSnapshot snapshot() const noexcept {
    return mapping_snapshot(mapping_);
  }
  [[nodiscard]] std::wstring name() const { return mapping_->name(); }

 private:
  std::shared_ptr<MappingLease> mapping_;
  CpuFrameConnectionId connection_id_{};
  std::mutex publish_mutex_;
};

CpuFrameMailboxSource::CpuFrameMailboxSource(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CpuFrameMailboxSource::~CpuFrameMailboxSource() { close(); }

std::optional<CpuNv12Frame> CpuFrameMailboxSource::take_latest(
    std::string& error) {
  std::shared_ptr<Impl> lease;
  {
    std::scoped_lock lock(mutex_);
    lease = impl_;
  }
  if (!lease) {
    error = "CPU frame mailbox source is closed";
    return std::nullopt;
  }
  return lease->take_latest(error);
}

CpuFrameMailboxSnapshot CpuFrameMailboxSource::snapshot() const {
  std::shared_ptr<Impl> lease;
  {
    std::scoped_lock lock(mutex_);
    lease = impl_;
  }
  return lease ? lease->snapshot() : CpuFrameMailboxSnapshot{};
}

std::wstring CpuFrameMailboxSource::name() const {
  std::shared_ptr<Impl> lease;
  {
    std::scoped_lock lock(mutex_);
    lease = impl_;
  }
  return lease ? lease->name() : std::wstring{};
}

bool CpuFrameMailboxSource::open() const {
  std::scoped_lock lock(mutex_);
  return static_cast<bool>(impl_);
}

void CpuFrameMailboxSource::close() noexcept {
  std::scoped_lock lock(mutex_);
  impl_.reset();
}

CpuFrameMailboxProducer::CpuFrameMailboxProducer(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CpuFrameMailboxProducer::~CpuFrameMailboxProducer() { close(); }

bool CpuFrameMailboxProducer::publish(const CpuNv12Frame& frame,
                                      std::string& error) {
  std::shared_ptr<Impl> lease;
  {
    std::scoped_lock lock(mutex_);
    lease = impl_;
  }
  if (!lease) {
    error = "CPU frame mailbox producer is closed";
    return false;
  }
  return lease->publish(frame, error);
}

CpuFrameMailboxSnapshot CpuFrameMailboxProducer::snapshot() const {
  std::shared_ptr<Impl> lease;
  {
    std::scoped_lock lock(mutex_);
    lease = impl_;
  }
  return lease ? lease->snapshot() : CpuFrameMailboxSnapshot{};
}

std::wstring CpuFrameMailboxProducer::name() const {
  std::shared_ptr<Impl> lease;
  {
    std::scoped_lock lock(mutex_);
    lease = impl_;
  }
  return lease ? lease->name() : std::wstring{};
}

bool CpuFrameMailboxProducer::open() const {
  std::scoped_lock lock(mutex_);
  return static_cast<bool>(impl_);
}

void CpuFrameMailboxProducer::close() noexcept {
  std::scoped_lock lock(mutex_);
  impl_.reset();
}

std::shared_ptr<CpuFrameMailboxSource> create_cpu_frame_mailbox_source(
    const CpuFrameMailboxOptions& options, std::string& error) {
  auto mapping = create_mapping(options, error);
  if (!mapping) return nullptr;
  auto impl = std::make_shared<CpuFrameMailboxSource::Impl>(
      std::move(mapping), options.connection_id);
  error.clear();
  return std::shared_ptr<CpuFrameMailboxSource>(
      new CpuFrameMailboxSource(std::move(impl)));
}

std::shared_ptr<CpuFrameMailboxProducer> open_cpu_frame_mailbox_producer(
    const CpuFrameMailboxOptions& options, std::string& error) {
  auto mapping = open_mapping(options, error);
  if (!mapping) return nullptr;
  auto impl = std::make_shared<CpuFrameMailboxProducer::Impl>(
      std::move(mapping), options.connection_id);
  error.clear();
  return std::shared_ptr<CpuFrameMailboxProducer>(
      new CpuFrameMailboxProducer(std::move(impl)));
}

} // namespace vividcam
