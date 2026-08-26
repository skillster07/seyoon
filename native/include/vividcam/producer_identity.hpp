#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace vividcam {

inline constexpr std::uint32_t kProducerIdentityManifestSchemaVersion = 1;
inline constexpr std::size_t kProducerIdentitySha256Bytes = 32;

using ProducerIdentitySha256 =
    std::array<std::uint8_t, kProducerIdentitySha256Bytes>;

struct ProducerIdentityManifest {
  std::uint32_t schema_version{0};
  std::uint64_t generation{0};
  std::wstring engine_path;
  std::wstring engine_user_sid;
  ProducerIdentitySha256 sha256{};

  [[nodiscard]] bool valid() const noexcept {
    if (schema_version != kProducerIdentityManifestSchemaVersion ||
        generation == 0 || engine_path.empty() || engine_user_sid.empty()) {
      return false;
    }
    for (const std::uint8_t byte : sha256) {
      if (byte != 0) return true;
    }
    return false;
  }
};

// Loads the machine-scoped identity manifest installed by the elevated
// VIVIDCAM installer. Implementations fail closed if either the registry value
// contract or the registry key's security descriptor differs from the
// documented production policy.
[[nodiscard]] bool load_installed_vividcam_producer_identity(
    ProducerIdentityManifest& manifest, std::string& error);

// Resolves vividcam_engine.exe beside the module that contains this function.
// In the virtual-camera source this anchors the expected package directory to
// the installed source DLL rather than to mutable process environment state.
[[nodiscard]] bool current_module_sibling_vividcam_engine_path(
    std::wstring& path, std::string& error);

[[nodiscard]] bool hash_vividcam_file_sha256(
    const std::wstring& path, ProducerIdentitySha256& hash,
    std::string& error);

// Requires all three paths (manifest, observed process, and package sibling)
// to match case-insensitively, then hashes the observed file and compares the
// result with the installed digest without data-dependent early exit.
[[nodiscard]] bool verify_vividcam_producer_image(
    const ProducerIdentityManifest& manifest,
    const std::wstring& observed_process_path,
    const std::wstring& expected_package_path, std::string& error);

} // namespace vividcam
