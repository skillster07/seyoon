#include "vividcam/cpu_frame_transport.hpp"

#include <algorithm>
#include <cwctype>

namespace vividcam {
namespace {

bool is_hex_digit(wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
         (value >= L'a' && value <= L'f') ||
         (value >= L'A' && value <= L'F');
}

bool nonzero_connection_id(const CpuFrameConnectionId& value) noexcept {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

wchar_t lower_hex(std::uint8_t nibble) noexcept {
  return static_cast<wchar_t>(nibble < 10U ? L'0' + nibble
                                           : L'a' + (nibble - 10U));
}

} // namespace

bool CpuNv12Frame::valid() const noexcept {
  return sequence != 0 && timestamp_100ns >= 0 && width == kCpuFrameWidth &&
         height == kCpuFrameHeight &&
         y_stride_bytes == kCpuFrameYStrideBytes &&
         uv_stride_bytes == kCpuFrameUvStrideBytes &&
         bytes.size() == kCpuFrameNv12Bytes;
}

bool make_cpu_frame_mailbox_name(const CpuFrameMailboxOptions& options,
                                 std::wstring& name, std::string& error) {
  name.clear();
  if (options.route_digest.size() != 64U ||
      !std::all_of(options.route_digest.begin(), options.route_digest.end(),
                   is_hex_digit)) {
    error = "CPU frame mailbox route digest must contain exactly 64 hex digits";
    return false;
  }
  if (!nonzero_connection_id(options.connection_id)) {
    error = "CPU frame mailbox connection ID must not be zero";
    return false;
  }

  std::wstring canonical_digest(options.route_digest);
  std::transform(canonical_digest.begin(), canonical_digest.end(),
                 canonical_digest.begin(), [](wchar_t character) {
                   return static_cast<wchar_t>(std::towlower(character));
                 });
  std::wstring connection_hex;
  connection_hex.reserve(options.connection_id.size() * 2U);
  for (const std::uint8_t byte : options.connection_id) {
    connection_hex.push_back(lower_hex(static_cast<std::uint8_t>(byte >> 4U)));
    connection_hex.push_back(lower_hex(static_cast<std::uint8_t>(byte & 0x0fU)));
  }

  switch (options.scope) {
    case CpuFrameMailboxScope::NonProductionLocal:
    case CpuFrameMailboxScope::ProductionSecurityLocalTest:
      name = L"Local\\VIVIDCAM.Frame.v1.";
      break;
    case CpuFrameMailboxScope::ProductionGlobal:
      name = L"Global\\VIVIDCAM.Frame.v1.";
      break;
    default:
      error = "CPU frame mailbox scope is invalid";
      return false;
  }
  name += canonical_digest;
  name.push_back(L'.');
  name += connection_hex;
  error.clear();
  return true;
}

} // namespace vividcam
