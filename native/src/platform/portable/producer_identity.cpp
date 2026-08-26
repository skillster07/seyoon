#include "vividcam/producer_identity.hpp"

namespace vividcam {
namespace {

constexpr const char* kUnsupported =
    "VIVIDCAM producer identity is only available on Windows";

} // namespace

bool load_installed_vividcam_producer_identity(
    ProducerIdentityManifest& manifest, std::string& error) {
  manifest = {};
  error = kUnsupported;
  return false;
}

bool current_module_sibling_vividcam_engine_path(std::wstring& path,
                                                  std::string& error) {
  path.clear();
  error = kUnsupported;
  return false;
}

bool hash_vividcam_file_sha256(const std::wstring&,
                               ProducerIdentitySha256& hash,
                               std::string& error) {
  hash.fill(0);
  error = kUnsupported;
  return false;
}

bool verify_vividcam_producer_image(
    const ProducerIdentityManifest&, const std::wstring&,
    const std::wstring&, std::string& error) {
  error = kUnsupported;
  return false;
}

} // namespace vividcam
