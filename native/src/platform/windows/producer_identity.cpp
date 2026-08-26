#include "vividcam/producer_identity.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vividcam {
namespace {

constexpr wchar_t kProducerIdentityKey[] =
    L"Software\\VIVIDCAM\\VirtualCamera\\ProducerIdentity";
constexpr wchar_t kSchemaVersionValue[] = L"SchemaVersion";
constexpr wchar_t kGenerationValue[] = L"Generation";
constexpr wchar_t kEnginePathValue[] = L"EnginePath";
constexpr wchar_t kEngineUserSidValue[] = L"EngineUserSid";
constexpr wchar_t kEngineSha256Value[] = L"EngineSha256";
constexpr wchar_t kFrameServerAccount[] = L"NT SERVICE\\FrameServer";
constexpr wchar_t kEngineFilename[] = L"vividcam_engine.exe";
constexpr std::size_t kMaximumWindowsPathCharacters = 32767;
constexpr std::size_t kFileReadBufferBytes = 64U * 1024U;
const int kModuleAddressAnchor = 0;

class UniqueHandle {
 public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueHandle() { reset(); }
  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;
  UniqueHandle(UniqueHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.handle_, INVALID_HANDLE_VALUE));
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }
  void reset(HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
    if (valid()) CloseHandle(handle_);
    handle_ = replacement;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

class UniqueRegistryKey {
 public:
  UniqueRegistryKey() noexcept = default;
  explicit UniqueRegistryKey(HKEY key) noexcept : key_(key) {}
  ~UniqueRegistryKey() {
    if (key_ != nullptr) RegCloseKey(key_);
  }
  UniqueRegistryKey(const UniqueRegistryKey&) = delete;
  UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;

  [[nodiscard]] HKEY get() const noexcept { return key_; }

 private:
  HKEY key_{nullptr};
};

class LocalAllocation {
 public:
  explicit LocalAllocation(HLOCAL value) noexcept : value_(value) {}
  ~LocalAllocation() {
    if (value_ != nullptr) LocalFree(value_);
  }
  LocalAllocation(const LocalAllocation&) = delete;
  LocalAllocation& operator=(const LocalAllocation&) = delete;

 private:
  HLOCAL value_{nullptr};
};

class UniqueAlgorithm {
 public:
  UniqueAlgorithm() noexcept = default;
  ~UniqueAlgorithm() {
    if (value_ != nullptr) BCryptCloseAlgorithmProvider(value_, 0);
  }
  UniqueAlgorithm(const UniqueAlgorithm&) = delete;
  UniqueAlgorithm& operator=(const UniqueAlgorithm&) = delete;

  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
  [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }

 private:
  BCRYPT_ALG_HANDLE value_{nullptr};
};

class UniqueHash {
 public:
  UniqueHash() noexcept = default;
  ~UniqueHash() {
    if (value_ != nullptr) BCryptDestroyHash(value_);
  }
  UniqueHash(const UniqueHash&) = delete;
  UniqueHash& operator=(const UniqueHash&) = delete;

  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }
  [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }

 private:
  BCRYPT_HASH_HANDLE value_{nullptr};
};

std::string windows_error(const char* operation, DWORD status) {
  std::ostringstream message;
  message << operation << " failed (Win32=" << status << ')';
  return message.str();
}

std::string ntstatus_error(const char* operation, NTSTATUS status) {
  std::ostringstream message;
  message << operation << " failed (NTSTATUS=0x" << std::hex << std::uppercase
          << std::setw(8) << std::setfill('0')
          << static_cast<std::uint32_t>(status) << ')';
  return message.str();
}

bool create_well_known_sid(WELL_KNOWN_SID_TYPE type,
                           std::vector<std::byte>& sid,
                           std::string& error) {
  sid.assign(SECURITY_MAX_SID_SIZE, std::byte{0});
  DWORD bytes = static_cast<DWORD>(sid.size());
  if (!CreateWellKnownSid(type, nullptr, sid.data(), &bytes)) {
    error = windows_error("CreateWellKnownSid", GetLastError());
    sid.clear();
    return false;
  }
  sid.resize(bytes);
  error.clear();
  return true;
}

bool lookup_account_sid(const wchar_t* account,
                        std::vector<std::byte>& sid,
                        std::string& error) {
  DWORD sid_bytes = 0;
  DWORD domain_characters = 0;
  SID_NAME_USE use = SidTypeUnknown;
  SetLastError(ERROR_SUCCESS);
  if (LookupAccountNameW(nullptr, account, nullptr, &sid_bytes, nullptr,
                         &domain_characters, &use) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || sid_bytes == 0) {
    error = windows_error("LookupAccountName(size)", GetLastError());
    return false;
  }

  sid.assign(sid_bytes, std::byte{0});
  std::vector<wchar_t> domain(
      std::max<DWORD>(domain_characters, static_cast<DWORD>(1)), L'\0');
  if (!LookupAccountNameW(nullptr, account, sid.data(), &sid_bytes,
                          domain.data(), &domain_characters, &use)) {
    error = windows_error("LookupAccountName", GetLastError());
    sid.clear();
    return false;
  }
  sid.resize(sid_bytes);
  if (!IsValidSid(sid.data())) {
    error = "LookupAccountName returned an invalid service SID";
    sid.clear();
    return false;
  }
  error.clear();
  return true;
}

bool ace_sid(const ACE_HEADER* header, PSID& sid, std::string& error) {
  sid = nullptr;
  constexpr std::size_t kSidOffset = offsetof(ACCESS_ALLOWED_ACE, SidStart);
  constexpr std::size_t kMinimumSidBytes = 8;
  if (header == nullptr ||
      header->AceSize < kSidOffset + kMinimumSidBytes) {
    error = "Producer identity registry key contains a malformed ACE";
    return false;
  }

  const auto* allowed = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(header);
  auto* candidate = const_cast<DWORD*>(&allowed->SidStart);
  const auto* sid_header = reinterpret_cast<const SID*>(candidate);
  const DWORD sid_bytes = GetSidLengthRequired(sid_header->SubAuthorityCount);
  if (sid_bytes > header->AceSize - kSidOffset || !IsValidSid(candidate)) {
    error = "Producer identity registry key contains an invalid ACE SID";
    return false;
  }
  sid = candidate;
  error.clear();
  return true;
}

struct ExpectedRegistryAce {
  PSID sid{nullptr};
  ACCESS_MASK mask{0};
  bool found{false};
};

bool validate_registry_key_security(HKEY key, std::string& error) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  const DWORD status = GetSecurityInfo(
      key, SE_REGISTRY_KEY,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &raw_descriptor);
  if (status != ERROR_SUCCESS) {
    error = windows_error("GetSecurityInfo(producer identity key)", status);
    return false;
  }
  LocalAllocation descriptor_owner(
      reinterpret_cast<HLOCAL>(raw_descriptor));
  if (raw_descriptor == nullptr || owner == nullptr || !IsValidSid(owner) ||
      dacl == nullptr || !IsValidAcl(dacl)) {
    error = "Producer identity registry key has an invalid security descriptor";
    return false;
  }

  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!GetSecurityDescriptorControl(raw_descriptor, &control, &revision)) {
    error = windows_error("GetSecurityDescriptorControl(producer identity key)",
                          GetLastError());
    return false;
  }
  if ((control & SE_DACL_PROTECTED) == 0) {
    error = "Producer identity registry key DACL is not protected";
    return false;
  }

  std::vector<std::byte> system_sid;
  std::vector<std::byte> administrators_sid;
  std::vector<std::byte> frame_server_sid;
  if (!create_well_known_sid(WinLocalSystemSid, system_sid, error) ||
      !create_well_known_sid(WinBuiltinAdministratorsSid,
                             administrators_sid, error) ||
      !lookup_account_sid(kFrameServerAccount, frame_server_sid, error)) {
    return false;
  }
  if (EqualSid(owner, system_sid.data()) == FALSE &&
      EqualSid(owner, administrators_sid.data()) == FALSE) {
    error =
        "Producer identity registry key owner is not SYSTEM or Administrators";
    return false;
  }

  if (dacl->AceCount != 3) {
    error = "Producer identity registry key must have exactly three ACEs";
    return false;
  }
  std::array<ExpectedRegistryAce, 3> expected{{
      {system_sid.data(), KEY_ALL_ACCESS, false},
      {administrators_sid.data(), KEY_ALL_ACCESS, false},
      {frame_server_sid.data(), KEY_QUERY_VALUE | READ_CONTROL, false},
  }};

  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (!GetAce(dacl, index, &raw_ace)) {
      error = windows_error("GetAce(producer identity key)", GetLastError());
      return false;
    }
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      error =
          "Producer identity registry key contains a deny or unknown ACE";
      return false;
    }
    if (header->AceFlags != 0) {
      error =
          "Producer identity registry key contains an inherited or flagged ACE";
      return false;
    }

    PSID sid = nullptr;
    if (!ace_sid(header, sid, error)) return false;
    const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    auto match = std::find_if(
        expected.begin(), expected.end(),
        [sid](const ExpectedRegistryAce& candidate) {
          return EqualSid(sid, candidate.sid) != FALSE;
        });
    if (match == expected.end() || match->found ||
        allowed->Mask != match->mask) {
      error =
          "Producer identity registry key ACE principal or access mask is invalid";
      return false;
    }
    match->found = true;
  }

  if (!std::all_of(expected.begin(), expected.end(),
                   [](const ExpectedRegistryAce& entry) {
                     return entry.found;
                   })) {
    error = "Producer identity registry key is missing a required ACE";
    return false;
  }
  error.clear();
  return true;
}

bool query_fixed_registry_value(HKEY key, const wchar_t* name,
                                DWORD expected_type, void* output,
                                DWORD expected_bytes,
                                std::string& error) {
  DWORD type = REG_NONE;
  DWORD bytes = expected_bytes;
  const LSTATUS status = RegQueryValueExW(
      key, name, nullptr, &type, static_cast<BYTE*>(output), &bytes);
  if (status != ERROR_SUCCESS) {
    error = windows_error("RegQueryValueEx(producer identity)",
                          static_cast<DWORD>(status));
    return false;
  }
  if (type != expected_type || bytes != expected_bytes) {
    error = "Producer identity registry value has an invalid type or length";
    return false;
  }
  error.clear();
  return true;
}

bool query_registry_string(HKEY key, const wchar_t* value_name,
                           const char* value_description,
                           std::wstring& value, std::string& error) {
  value.clear();
  DWORD type = REG_NONE;
  DWORD bytes = 0;
  LSTATUS status = RegQueryValueExW(key, value_name, nullptr, &type,
                                    nullptr, &bytes);
  if (status != ERROR_SUCCESS) {
    error = windows_error(value_description, static_cast<DWORD>(status));
    return false;
  }
  const DWORD maximum_bytes = static_cast<DWORD>(
      (kMaximumWindowsPathCharacters + 1) * sizeof(wchar_t));
  if (type != REG_SZ || bytes < sizeof(wchar_t) ||
      bytes > maximum_bytes || bytes % sizeof(wchar_t) != 0) {
    error = "Producer identity string has an invalid type or length";
    return false;
  }

  std::vector<wchar_t> storage(bytes / sizeof(wchar_t), L'\0');
  DWORD read_type = REG_NONE;
  DWORD read_bytes = bytes;
  status = RegQueryValueExW(key, value_name, nullptr, &read_type,
                            reinterpret_cast<BYTE*>(storage.data()),
                            &read_bytes);
  if (status != ERROR_SUCCESS) {
    error = windows_error(value_description, static_cast<DWORD>(status));
    return false;
  }
  if (read_type != REG_SZ || read_bytes != bytes || storage.back() != L'\0' ||
      std::find(storage.begin(), storage.end() - 1, L'\0') !=
          storage.end() - 1) {
    error = "Producer identity value is not one exact REG_SZ string";
    return false;
  }
  value.assign(storage.data(), storage.size() - 1);
  error.clear();
  return true;
}

bool paths_equal(const std::wstring& left, const std::wstring& right) noexcept {
  if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(), static_cast<int>(left.size()), right.data(),
             static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool validate_canonical_absolute_path(const std::wstring& path,
                                      std::string& error) {
  if (path.size() < 3 || path.size() > kMaximumWindowsPathCharacters ||
      path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
    error = "Producer identity path is not an absolute drive path";
    return false;
  }

  const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0 || required > kMaximumWindowsPathCharacters + 1) {
    error = windows_error("GetFullPathName(producer identity)", GetLastError());
    return false;
  }
  std::vector<wchar_t> storage(required, L'\0');
  const DWORD characters = GetFullPathNameW(
      path.c_str(), static_cast<DWORD>(storage.size()), storage.data(), nullptr);
  if (characters == 0 || characters >= storage.size()) {
    error = characters == 0
                ? windows_error("GetFullPathName(producer identity)",
                                GetLastError())
                : "Producer identity path exceeded its canonical buffer";
    return false;
  }
  const std::wstring canonical(storage.data(), characters);
  if (!paths_equal(path, canonical)) {
    error = "Producer identity path is not canonical";
    return false;
  }
  error.clear();
  return true;
}

bool resolve_final_regular_file_path(const std::wstring& path,
                                     std::wstring& final_path,
                                     std::string& error) {
  final_path.clear();
  if (!validate_canonical_absolute_path(path, error)) return false;
  UniqueHandle file(CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file.valid()) {
    error = windows_error("CreateFile(resolve producer image)",
                          GetLastError());
    return false;
  }
  if (GetFileType(file.get()) != FILE_TYPE_DISK) {
    error = "Producer image is not a disk file";
    return false;
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo,
                                    &attributes, sizeof(attributes))) {
    error = windows_error("GetFileInformationByHandleEx(producer image)",
                          GetLastError());
    return false;
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    error = "Producer image must be a regular non-reparse file";
    return false;
  }
  const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
  const DWORD required = GetFinalPathNameByHandleW(file.get(), nullptr, 0,
                                                   flags);
  if (required == 0 || required > kMaximumWindowsPathCharacters + 4) {
    error = windows_error("GetFinalPathNameByHandle(producer image)",
                          GetLastError());
    return false;
  }
  std::vector<wchar_t> storage(static_cast<std::size_t>(required) + 1, L'\0');
  const DWORD characters = GetFinalPathNameByHandleW(
      file.get(), storage.data(), static_cast<DWORD>(storage.size()), flags);
  if (characters == 0 || characters >= storage.size()) {
    error = characters == 0
                ? windows_error("GetFinalPathNameByHandle(producer image)",
                                GetLastError())
                : "Resolved producer image path exceeded its buffer";
    return false;
  }
  final_path.assign(storage.data(), characters);
  error.clear();
  return true;
}

bool validate_canonical_string_sid(const std::wstring& text,
                                   std::string& error) {
  PSID raw_sid = nullptr;
  if (!ConvertStringSidToSidW(text.c_str(), &raw_sid)) {
    error = windows_error("ConvertStringSidToSid(EngineUserSid)",
                          GetLastError());
    return false;
  }
  LocalAllocation sid_owner(reinterpret_cast<HLOCAL>(raw_sid));
  if (!IsValidSid(raw_sid)) {
    error = "Producer identity EngineUserSid is invalid";
    return false;
  }
  wchar_t* raw_canonical = nullptr;
  if (!ConvertSidToStringSidW(raw_sid, &raw_canonical)) {
    error = windows_error("ConvertSidToStringSid(EngineUserSid)",
                          GetLastError());
    return false;
  }
  LocalAllocation canonical_owner(reinterpret_cast<HLOCAL>(raw_canonical));
  if (!paths_equal(text, raw_canonical)) {
    error = "Producer identity EngineUserSid is not canonical";
    return false;
  }
  error.clear();
  return true;
}

} // namespace

bool load_installed_vividcam_producer_identity(
    ProducerIdentityManifest& manifest, std::string& error) {
  manifest = {};
  HKEY raw_key = nullptr;
  const LSTATUS status = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE, kProducerIdentityKey, 0,
      KEY_QUERY_VALUE | READ_CONTROL | KEY_WOW64_64KEY, &raw_key);
  if (status != ERROR_SUCCESS) {
    error = windows_error("RegOpenKeyEx(producer identity)",
                          static_cast<DWORD>(status));
    return false;
  }
  UniqueRegistryKey key(raw_key);
  if (!validate_registry_key_security(key.get(), error)) return false;

  std::uint64_t generation_before = 0;
  std::uint64_t generation_after = 0;
  if (!query_fixed_registry_value(
          key.get(), kGenerationValue, REG_QWORD, &generation_before,
          static_cast<DWORD>(sizeof(generation_before)), error) ||
      !query_fixed_registry_value(
          key.get(), kSchemaVersionValue, REG_DWORD,
          &manifest.schema_version,
          static_cast<DWORD>(sizeof(manifest.schema_version)), error) ||
      !query_registry_string(key.get(), kEnginePathValue,
                             "RegQueryValueEx(EnginePath)",
                             manifest.engine_path, error) ||
      !query_registry_string(key.get(), kEngineUserSidValue,
                             "RegQueryValueEx(EngineUserSid)",
                             manifest.engine_user_sid, error) ||
      !query_fixed_registry_value(
          key.get(), kEngineSha256Value, REG_BINARY, manifest.sha256.data(),
          static_cast<DWORD>(manifest.sha256.size()), error) ||
      !query_fixed_registry_value(
          key.get(), kGenerationValue, REG_QWORD, &generation_after,
          static_cast<DWORD>(sizeof(generation_after)), error)) {
    manifest = {};
    return false;
  }
  if (generation_before != generation_after) {
    manifest = {};
    error = "Producer identity manifest changed while it was being read";
    return false;
  }
  manifest.generation = generation_after;
  if (!manifest.valid()) {
    manifest = {};
    error = "Installed producer identity manifest is invalid";
    return false;
  }
  if (!validate_canonical_absolute_path(manifest.engine_path, error)) {
    manifest = {};
    return false;
  }
  if (!validate_canonical_string_sid(manifest.engine_user_sid, error)) {
    manifest = {};
    return false;
  }
  error.clear();
  return true;
}

bool current_module_sibling_vividcam_engine_path(std::wstring& path,
                                                  std::string& error) {
  path.clear();
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&kModuleAddressAnchor), &module)) {
    error = windows_error("GetModuleHandleEx(producer identity)",
                          GetLastError());
    return false;
  }

  std::vector<wchar_t> storage(MAX_PATH, L'\0');
  std::wstring module_path;
  while (storage.size() <= kMaximumWindowsPathCharacters) {
    SetLastError(ERROR_SUCCESS);
    const DWORD characters = GetModuleFileNameW(
        module, storage.data(), static_cast<DWORD>(storage.size()));
    if (characters == 0) {
      error = windows_error("GetModuleFileName(producer identity)",
                            GetLastError());
      return false;
    }
    if (characters < storage.size()) {
      module_path.assign(storage.data(), characters);
      break;
    }
    const std::size_t expanded =
        std::min<std::size_t>(storage.size() * 2,
                              kMaximumWindowsPathCharacters + 1);
    if (expanded <= storage.size()) break;
    storage.assign(expanded, L'\0');
  }
  if (module_path.empty()) {
    error = "The current module path exceeds the Windows path limit";
    return false;
  }

  const std::size_t separator = module_path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    error = "The current module path has no package directory";
    return false;
  }
  path.assign(module_path, 0, separator + 1);
  path.append(kEngineFilename);
  if (!validate_canonical_absolute_path(path, error)) {
    path.clear();
    return false;
  }
  error.clear();
  return true;
}

bool hash_vividcam_file_sha256(const std::wstring& path,
                               ProducerIdentitySha256& hash,
                               std::string& error) {
  hash.fill(0);
  if (!validate_canonical_absolute_path(path, error)) return false;

  UniqueHandle file(CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
          FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!file.valid()) {
    error = windows_error("CreateFile(producer image)", GetLastError());
    return false;
  }
  if (GetFileType(file.get()) != FILE_TYPE_DISK) {
    error = "Producer image is not a disk file";
    return false;
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo,
                                    &attributes, sizeof(attributes))) {
    error = windows_error("GetFileInformationByHandleEx(producer image)",
                          GetLastError());
    return false;
  }
  if ((attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    error = "Producer image must be a regular non-reparse file";
    return false;
  }

  UniqueAlgorithm algorithm;
  NTSTATUS crypto_status = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(crypto_status)) {
    error = ntstatus_error("BCryptOpenAlgorithmProvider(SHA-256)",
                           crypto_status);
    return false;
  }

  DWORD object_bytes = 0;
  DWORD result_bytes = 0;
  crypto_status = BCryptGetProperty(
      algorithm.get(), BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
      &result_bytes, 0);
  if (!BCRYPT_SUCCESS(crypto_status) || result_bytes != sizeof(object_bytes) ||
      object_bytes == 0) {
    error = BCRYPT_SUCCESS(crypto_status)
                ? "BCrypt returned an invalid SHA-256 object length"
                : ntstatus_error("BCryptGetProperty(object length)",
                                 crypto_status);
    return false;
  }
  DWORD digest_bytes = 0;
  crypto_status = BCryptGetProperty(
      algorithm.get(), BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&digest_bytes), sizeof(digest_bytes),
      &result_bytes, 0);
  if (!BCRYPT_SUCCESS(crypto_status) || result_bytes != sizeof(digest_bytes) ||
      digest_bytes != hash.size()) {
    error = BCRYPT_SUCCESS(crypto_status)
                ? "BCrypt SHA-256 digest length is not 32 bytes"
                : ntstatus_error("BCryptGetProperty(hash length)",
                                 crypto_status);
    return false;
  }

  std::vector<std::uint8_t> hash_object(object_bytes, 0);
  UniqueHash hash_handle;
  crypto_status = BCryptCreateHash(
      algorithm.get(), hash_handle.put(), hash_object.data(), object_bytes,
      nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(crypto_status)) {
    error = ntstatus_error("BCryptCreateHash(SHA-256)", crypto_status);
    return false;
  }

  std::array<std::uint8_t, kFileReadBufferBytes> buffer{};
  for (;;) {
    DWORD bytes_read = 0;
    if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                  &bytes_read, nullptr)) {
      error = windows_error("ReadFile(producer image)", GetLastError());
      return false;
    }
    if (bytes_read == 0) break;
    crypto_status =
        BCryptHashData(hash_handle.get(), buffer.data(), bytes_read, 0);
    if (!BCRYPT_SUCCESS(crypto_status)) {
      error = ntstatus_error("BCryptHashData(producer image)", crypto_status);
      return false;
    }
  }
  crypto_status = BCryptFinishHash(
      hash_handle.get(), hash.data(), static_cast<ULONG>(hash.size()), 0);
  if (!BCRYPT_SUCCESS(crypto_status)) {
    hash.fill(0);
    error = ntstatus_error("BCryptFinishHash(producer image)", crypto_status);
    return false;
  }
  error.clear();
  return true;
}

bool verify_vividcam_producer_image(
    const ProducerIdentityManifest& manifest,
    const std::wstring& observed_process_path,
    const std::wstring& expected_package_path, std::string& error) {
  if (!manifest.valid()) {
    error = "Producer identity manifest is invalid";
    return false;
  }
  if (!validate_canonical_absolute_path(manifest.engine_path, error) ||
      !validate_canonical_absolute_path(observed_process_path, error) ||
      !validate_canonical_absolute_path(expected_package_path, error)) {
    return false;
  }
  if (!paths_equal(manifest.engine_path, expected_package_path) ||
      !paths_equal(manifest.engine_path, observed_process_path)) {
    error =
        "Observed producer image is not the installed package sibling engine";
    return false;
  }

  std::wstring manifest_final_path;
  std::wstring observed_final_path;
  std::wstring package_final_path;
  if (!resolve_final_regular_file_path(manifest.engine_path,
                                       manifest_final_path, error) ||
      !resolve_final_regular_file_path(observed_process_path,
                                       observed_final_path, error) ||
      !resolve_final_regular_file_path(expected_package_path,
                                       package_final_path, error)) {
    return false;
  }
  if (!paths_equal(manifest_final_path, observed_final_path) ||
      !paths_equal(manifest_final_path, package_final_path)) {
    error = "Producer image paths do not resolve to the same installed file";
    return false;
  }

  ProducerIdentitySha256 observed_hash{};
  if (!hash_vividcam_file_sha256(observed_process_path, observed_hash, error)) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < observed_hash.size(); ++index) {
    difference = static_cast<std::uint8_t>(
        difference | (observed_hash[index] ^ manifest.sha256[index]));
  }
  if (difference != 0) {
    error = "Observed producer image SHA-256 does not match the manifest";
    return false;
  }
  error.clear();
  return true;
}

} // namespace vividcam
