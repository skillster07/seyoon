#include "vividcam/producer_identity.hpp"

#include <cassert>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <vector>
#endif

int main() {
  using vividcam::ProducerIdentityManifest;
  using vividcam::ProducerIdentitySha256;

  std::string error;
#ifdef _WIN32
  std::vector<wchar_t> path_storage(32768, L'\0');
  const DWORD path_characters = GetModuleFileNameW(
      nullptr, path_storage.data(), static_cast<DWORD>(path_storage.size()));
  assert(path_characters > 0);
  assert(path_characters < path_storage.size());
  const std::wstring executable_path(path_storage.data(), path_characters);

  ProducerIdentitySha256 executable_hash{};
  assert(vividcam::hash_vividcam_file_sha256(executable_path, executable_hash,
                                             error));
  assert(error.empty());

  ProducerIdentityManifest manifest;
  manifest.schema_version =
      vividcam::kProducerIdentityManifestSchemaVersion;
  manifest.generation = 1;
  manifest.engine_path = executable_path;
  assert(!manifest.valid());
  manifest.engine_user_sid = L"S-1-5-21-1-2-3-1001";
  manifest.sha256 = executable_hash;
  assert(manifest.valid());
  assert(vividcam::verify_vividcam_producer_image(
      manifest, executable_path, executable_path, error));
  assert(error.empty());

  ProducerIdentityManifest wrong_hash = manifest;
  wrong_hash.sha256[0] ^= 0xffU;
  assert(!vividcam::verify_vividcam_producer_image(
      wrong_hash, executable_path, executable_path, error));
  assert(!error.empty());

  const std::wstring wrong_path = executable_path + L".other";
  assert(!vividcam::verify_vividcam_producer_image(
      manifest, executable_path, wrong_path, error));
  assert(!error.empty());

  ProducerIdentityManifest invalid_manifest = manifest;
  invalid_manifest.generation = 0;
  assert(!invalid_manifest.valid());
  assert(!vividcam::verify_vividcam_producer_image(
      invalid_manifest, executable_path, executable_path, error));
  assert(!error.empty());

  invalid_manifest = manifest;
  invalid_manifest.sha256.fill(0);
  assert(!invalid_manifest.valid());

  std::wstring sibling_engine_path;
  assert(vividcam::current_module_sibling_vividcam_engine_path(
      sibling_engine_path, error));
  assert(sibling_engine_path.ends_with(L"\\vividcam_engine.exe"));
  assert(error.empty());
#else
  ProducerIdentityManifest manifest;
  assert(!vividcam::load_installed_vividcam_producer_identity(manifest, error));
  assert(!error.empty());

  std::wstring sibling_engine_path;
  assert(!vividcam::current_module_sibling_vividcam_engine_path(
      sibling_engine_path, error));
  assert(!error.empty());

  ProducerIdentitySha256 hash{};
  assert(!vividcam::hash_vividcam_file_sha256(L"/tmp/vividcam_engine", hash,
                                              error));
  assert(!error.empty());
  assert(!vividcam::verify_vividcam_producer_image(
      manifest, L"/tmp/vividcam_engine", L"/tmp/vividcam_engine", error));
  assert(!error.empty());
#endif
  return 0;
}
