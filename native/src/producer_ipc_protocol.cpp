#include "vividcam/producer_ipc_protocol.hpp"

#include "vividcam/cpu_frame_transport.hpp"

#include <algorithm>
#include <limits>

namespace vividcam::producer_ipc {
namespace {

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kHeaderBytesOffset = 4;
constexpr std::size_t kProtocolMajorOffset = 6;
constexpr std::size_t kProtocolMinorOffset = 8;
constexpr std::size_t kMessageTypeOffset = 10;
constexpr std::size_t kFlagsOffset = 12;
constexpr std::size_t kPayloadBytesOffset = 16;
constexpr std::size_t kReserved0Offset = 20;
constexpr std::size_t kMessageSequenceOffset = 24;
constexpr std::size_t kCorrelationIdOffset = 32;
constexpr std::size_t kConnectionIdOffset = 40;
constexpr std::size_t kReserved1Offset = 56;

static_assert(kReserved1Offset + sizeof(std::uint64_t) == kHeaderBytes);

constexpr std::size_t kNegotiationSchemaOffset = 0;
constexpr std::size_t kNegotiationPayloadBytesOffset = 2;

constexpr std::size_t kOpenStreamIdOffset = 4;
constexpr std::size_t kOpenStreamWidthOffset = 8;
constexpr std::size_t kOpenStreamHeightOffset = 12;
constexpr std::size_t kOpenStreamFrameRateNumeratorOffset = 16;
constexpr std::size_t kOpenStreamFrameRateDenominatorOffset = 20;
constexpr std::size_t kOpenStreamPixelFormatOffset = 24;
constexpr std::size_t kOpenStreamPlane0StrideOffset = 28;
constexpr std::size_t kOpenStreamPlane1StrideOffset = 32;
constexpr std::size_t kOpenStreamFrameBytesOffset = 36;
constexpr std::size_t kOpenStreamFlagsOffset = 40;
constexpr std::size_t kOpenStreamReservedOffset = 44;

static_assert(kOpenStreamReservedOffset + sizeof(std::uint32_t) ==
              kOpenStreamPayloadBytes);

constexpr std::size_t kTransportKindOffset = 4;
constexpr std::size_t kTransportLayoutMajorOffset = 8;
constexpr std::size_t kTransportLayoutMinorOffset = 10;
constexpr std::size_t kTransportSlotCountOffset = 12;
constexpr std::size_t kTransportMappingHeaderBytesOffset = 16;
constexpr std::size_t kTransportFrameCapacityBytesOffset = 20;
constexpr std::size_t kTransportMappingCapacityBytesOffset = 24;
constexpr std::size_t kTransportFlagsOffset = 32;
constexpr std::size_t kTransportReservedOffset = 36;

static_assert(kTransportReservedOffset + sizeof(std::uint32_t) ==
              kTransportOfferPayloadBytes);
static_assert(kTransportOfferPayloadBytes == kTransportDescriptorPayloadBytes);
// The negotiation ABI and the mapped-memory ABI ship as one layout version.
// Keep their duplicated wire constants tied together at compile time.
static_assert(vividcam::kCpuFrameMailboxLayoutVersion ==
              kCpuFrameMailboxLayoutMajor);
static_assert(kCpuFrameMailboxLayoutMinor == 0);
static_assert(vividcam::kCpuFrameMailboxSlotCount ==
              kCpuFrameMailboxSlotCount);
static_assert(vividcam::cpu_frame_mailbox_layout::kHeaderBytes ==
              kCpuFrameMailboxHeaderBytes);
static_assert(vividcam::cpu_frame_mailbox_layout::kSlotHeaderBytes ==
              kCpuFrameMailboxSlotMetadataBytes);
static_assert(vividcam::cpu_frame_mailbox_layout::kPageBytes ==
              kCpuFrameMailboxAlignmentBytes);
static_assert(vividcam::kCpuFrameNv12Bytes <= kMaximumCpuFrameBytes);

std::uint16_t read_u16(std::span<const std::byte> source,
                       std::size_t offset) noexcept {
  const auto low = std::to_integer<std::uint8_t>(source[offset]);
  const auto high = std::to_integer<std::uint8_t>(source[offset + 1]);
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(low) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));
}

std::uint32_t read_u32(std::span<const std::byte> source,
                       std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(source[offset + index]))
             << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

std::uint64_t read_u64(std::span<const std::byte> source,
                       std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(source[offset + index]))
             << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

void write_u16(std::span<std::byte> destination, std::size_t offset,
               std::uint16_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

void write_u32(std::span<std::byte> destination, std::size_t offset,
               std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

void write_u64(std::span<std::byte> destination, std::size_t offset,
               std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> static_cast<unsigned int>(index * 8U)));
  }
}

bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
  result = left + right;
  return true;
}

bool checked_multiply(std::uint64_t left, std::uint64_t right,
                      std::uint64_t& result) noexcept {
  if (left != 0 &&
      right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool checked_align_up(std::uint64_t value, std::uint64_t alignment,
                      std::uint64_t& result) noexcept {
  if (alignment == 0) return false;
  const std::uint64_t remainder = value % alignment;
  if (remainder == 0) {
    result = value;
    return true;
  }
  return checked_add(value, alignment - remainder, result);
}

bool is_known_pixel_format(FramePixelFormat format) noexcept {
  switch (format) {
    case FramePixelFormat::Nv12:
    case FramePixelFormat::Bgra:
      return true;
  }
  return false;
}

bool is_known_transport_kind(FrameTransportKind kind) noexcept {
  return kind == FrameTransportKind::CpuSharedMemory;
}

NegotiationPayloadError validate_open_stream_payload(
    const OpenStreamPayload& payload) noexcept {
  if (payload.payload_bytes != kOpenStreamPayloadBytes) {
    return NegotiationPayloadError::WrongPayloadSize;
  }
  if (payload.schema_version != kNegotiationPayloadSchemaVersion) {
    return NegotiationPayloadError::UnsupportedSchemaVersion;
  }
  if (payload.stream_id != 0) {
    return NegotiationPayloadError::InvalidStreamId;
  }
  if (!is_known_pixel_format(payload.pixel_format)) {
    return NegotiationPayloadError::UnknownPixelFormat;
  }
  if (payload.width == 0 || payload.height == 0) {
    return NegotiationPayloadError::InvalidDimensions;
  }
  if (payload.frame_rate_numerator == 0 ||
      payload.frame_rate_denominator == 0) {
    return NegotiationPayloadError::InvalidFrameRate;
  }

  std::uint64_t pixels = 0;
  if (!checked_multiply(payload.width, payload.height, pixels)) {
    return NegotiationPayloadError::ArithmeticOverflow;
  }

  std::uint64_t expected_frame_bytes = 0;
  if (payload.pixel_format == FramePixelFormat::Nv12) {
    if ((payload.width & 1U) != 0 || (payload.height & 1U) != 0) {
      return NegotiationPayloadError::InvalidDimensions;
    }
    if (payload.plane0_stride_bytes != payload.width ||
        payload.plane1_stride_bytes != payload.width) {
      return NegotiationPayloadError::InvalidStride;
    }
    if (!checked_add(pixels, pixels / 2U, expected_frame_bytes)) {
      return NegotiationPayloadError::ArithmeticOverflow;
    }
  } else {
    std::uint64_t expected_stride = 0;
    if (!checked_multiply(payload.width, 4U, expected_stride) ||
        expected_stride > std::numeric_limits<std::uint32_t>::max()) {
      return NegotiationPayloadError::ArithmeticOverflow;
    }
    if (payload.plane0_stride_bytes != expected_stride ||
        payload.plane1_stride_bytes != 0) {
      return NegotiationPayloadError::InvalidStride;
    }
    if (!checked_multiply(pixels, 4U, expected_frame_bytes)) {
      return NegotiationPayloadError::ArithmeticOverflow;
    }
  }

  if (payload.width > kMaximumFrameDimension ||
      payload.height > kMaximumFrameDimension) {
    return NegotiationPayloadError::InvalidDimensions;
  }
  if (expected_frame_bytes == 0 ||
      expected_frame_bytes > kMaximumCpuFrameBytes ||
      expected_frame_bytes > std::numeric_limits<std::uint32_t>::max() ||
      payload.frame_bytes != expected_frame_bytes) {
    return NegotiationPayloadError::InvalidFrameBytes;
  }
  if (payload.flags != 0) return NegotiationPayloadError::NonzeroFlags;
  if (payload.reserved != 0) return NegotiationPayloadError::NonzeroReserved;
  return NegotiationPayloadError::None;
}

NegotiationPayloadError calculate_cpu_frame_mapping_capacity(
    std::uint32_t frame_capacity_bytes,
    std::uint64_t& mapping_capacity_bytes) noexcept {
  mapping_capacity_bytes = 0;
  if (frame_capacity_bytes == 0 ||
      frame_capacity_bytes > kMaximumCpuFrameBytes) {
    return NegotiationPayloadError::InvalidCapacity;
  }
  std::uint64_t slot_bytes = 0;
  if (!checked_add(kCpuFrameMailboxSlotMetadataBytes, frame_capacity_bytes,
                   slot_bytes) ||
      !checked_align_up(slot_bytes, kCpuFrameMailboxAlignmentBytes,
                        slot_bytes)) {
    return NegotiationPayloadError::ArithmeticOverflow;
  }
  std::uint64_t all_slots_bytes = 0;
  if (!checked_multiply(slot_bytes, kCpuFrameMailboxSlotCount,
                        all_slots_bytes) ||
      !checked_add(kCpuFrameMailboxHeaderBytes, all_slots_bytes,
                   mapping_capacity_bytes)) {
    return NegotiationPayloadError::ArithmeticOverflow;
  }
  return NegotiationPayloadError::None;
}

template <typename Payload>
NegotiationPayloadError validate_transport_payload(
    const Payload& payload, std::uint16_t expected_payload_bytes) noexcept {
  if (payload.payload_bytes != expected_payload_bytes) {
    return NegotiationPayloadError::WrongPayloadSize;
  }
  if (payload.schema_version != kNegotiationPayloadSchemaVersion) {
    return NegotiationPayloadError::UnsupportedSchemaVersion;
  }
  if (!is_known_transport_kind(payload.transport_kind)) {
    return NegotiationPayloadError::UnknownTransportKind;
  }
  if (payload.layout_major != kCpuFrameMailboxLayoutMajor ||
      payload.layout_minor != kCpuFrameMailboxLayoutMinor) {
    return NegotiationPayloadError::UnsupportedTransportLayout;
  }
  if (payload.slot_count != kCpuFrameMailboxSlotCount) {
    return NegotiationPayloadError::InvalidSlotCount;
  }
  if (payload.mapping_header_bytes != kCpuFrameMailboxHeaderBytes) {
    return NegotiationPayloadError::InvalidCapacity;
  }
  std::uint64_t expected_mapping_capacity = 0;
  const NegotiationPayloadError capacity_status =
      calculate_cpu_frame_mapping_capacity(payload.frame_capacity_bytes,
                                           expected_mapping_capacity);
  if (capacity_status != NegotiationPayloadError::None) {
    return capacity_status;
  }
  if (payload.mapping_capacity_bytes != expected_mapping_capacity) {
    return NegotiationPayloadError::InvalidCapacity;
  }
  if (payload.flags != 0) return NegotiationPayloadError::NonzeroFlags;
  if (payload.reserved != 0) return NegotiationPayloadError::NonzeroReserved;
  return NegotiationPayloadError::None;
}

template <typename Payload>
void encode_transport_fields(const Payload& payload,
                             std::span<std::byte> destination) noexcept {
  write_u16(destination, kNegotiationSchemaOffset, payload.schema_version);
  write_u16(destination, kNegotiationPayloadBytesOffset, payload.payload_bytes);
  write_u32(destination, kTransportKindOffset,
            static_cast<std::uint32_t>(payload.transport_kind));
  write_u16(destination, kTransportLayoutMajorOffset, payload.layout_major);
  write_u16(destination, kTransportLayoutMinorOffset, payload.layout_minor);
  write_u32(destination, kTransportSlotCountOffset, payload.slot_count);
  write_u32(destination, kTransportMappingHeaderBytesOffset,
            payload.mapping_header_bytes);
  write_u32(destination, kTransportFrameCapacityBytesOffset,
            payload.frame_capacity_bytes);
  write_u64(destination, kTransportMappingCapacityBytesOffset,
            payload.mapping_capacity_bytes);
  write_u32(destination, kTransportFlagsOffset, payload.flags);
  write_u32(destination, kTransportReservedOffset, payload.reserved);
}

template <typename Payload>
void decode_transport_fields(std::span<const std::byte> source,
                             Payload& payload) noexcept {
  payload.schema_version = read_u16(source, kNegotiationSchemaOffset);
  payload.payload_bytes = read_u16(source, kNegotiationPayloadBytesOffset);
  payload.transport_kind =
      static_cast<FrameTransportKind>(read_u32(source, kTransportKindOffset));
  payload.layout_major = read_u16(source, kTransportLayoutMajorOffset);
  payload.layout_minor = read_u16(source, kTransportLayoutMinorOffset);
  payload.slot_count = read_u32(source, kTransportSlotCountOffset);
  payload.mapping_header_bytes =
      read_u32(source, kTransportMappingHeaderBytesOffset);
  payload.frame_capacity_bytes =
      read_u32(source, kTransportFrameCapacityBytesOffset);
  payload.mapping_capacity_bytes =
      read_u64(source, kTransportMappingCapacityBytesOffset);
  payload.flags = read_u32(source, kTransportFlagsOffset);
  payload.reserved = read_u32(source, kTransportReservedOffset);
}

ProtocolError validate_header(const MessageHeader& header,
                              bool encoding) noexcept {
  if (header.header_bytes != kHeaderBytes) {
    return ProtocolError::WrongHeaderSize;
  }
  if (header.protocol_major != kProtocolMajor) {
    return ProtocolError::UnsupportedMajorVersion;
  }
  if (encoding && header.protocol_minor != kProtocolMinor) {
    return ProtocolError::UnsupportedMinorVersion;
  }
  if (!is_known_message_type(header.message_type)) {
    return ProtocolError::UnknownMessageType;
  }
  if (header.payload_bytes > kMaximumPayloadBytes) {
    return ProtocolError::PayloadTooLarge;
  }
  if (header.reserved0 != 0) {
    return ProtocolError::NonzeroReserved0;
  }
  if (header.reserved1 != 0) {
    return ProtocolError::NonzeroReserved1;
  }
  if (header.message_sequence == 0) {
    return ProtocolError::ZeroMessageSequence;
  }
  return ProtocolError::None;
}

} // namespace

bool is_known_message_type(MessageType type) noexcept {
  switch (type) {
    case MessageType::SourceHello:
    case MessageType::ProducerHello:
    case MessageType::OpenStream:
    case MessageType::StreamReady:
    case MessageType::StopStream:
    case MessageType::StreamStopped:
    case MessageType::ProducerState:
    case MessageType::Heartbeat:
    case MessageType::HeartbeatAck:
    case MessageType::TransportOffer:
    case MessageType::TransportAccepted:
    case MessageType::Error:
    case MessageType::Goodbye:
      return true;
  }
  return false;
}

std::string_view protocol_error_message(ProtocolError error) noexcept {
  switch (error) {
    case ProtocolError::None:
      return "no protocol error";
    case ProtocolError::OutputBufferTooSmall:
      return "output buffer is smaller than the 64-byte header";
    case ProtocolError::HeaderTruncated:
      return "message is shorter than the 64-byte header";
    case ProtocolError::WrongMagic:
      return "message magic is not VCIP";
    case ProtocolError::WrongHeaderSize:
      return "header_bytes is not 64";
    case ProtocolError::UnsupportedMajorVersion:
      return "protocol major version is not supported";
    case ProtocolError::UnsupportedMinorVersion:
      return "protocol minor version cannot be emitted by this codec";
    case ProtocolError::UnknownMessageType:
      return "message type is unknown";
    case ProtocolError::PayloadTooLarge:
      return "payload exceeds 64 KiB";
    case ProtocolError::PayloadSizeMismatch:
      return "payload size does not match payload_bytes";
    case ProtocolError::PayloadTruncated:
      return "message payload is truncated";
    case ProtocolError::TrailingBytes:
      return "message contains trailing bytes";
    case ProtocolError::NonzeroReserved0:
      return "reserved0 must be zero";
    case ProtocolError::NonzeroReserved1:
      return "reserved1 must be zero";
    case ProtocolError::ZeroMessageSequence:
      return "message_sequence must be nonzero";
  }
  return "unknown protocol error";
}

std::string_view negotiation_payload_error_message(
    NegotiationPayloadError error) noexcept {
  switch (error) {
    case NegotiationPayloadError::None:
      return "no negotiation payload error";
    case NegotiationPayloadError::WrongPayloadSize:
      return "negotiation payload size does not match its fixed contract";
    case NegotiationPayloadError::UnsupportedSchemaVersion:
      return "negotiation payload schema version is not supported";
    case NegotiationPayloadError::InvalidStreamId:
      return "OpenStream stream ID must be zero";
    case NegotiationPayloadError::UnknownPixelFormat:
      return "OpenStream pixel format is unknown";
    case NegotiationPayloadError::InvalidDimensions:
      return "OpenStream dimensions are invalid";
    case NegotiationPayloadError::InvalidFrameRate:
      return "OpenStream frame rate is invalid";
    case NegotiationPayloadError::InvalidStride:
      return "OpenStream plane strides do not match the pixel format";
    case NegotiationPayloadError::InvalidFrameBytes:
      return "OpenStream frame byte count is invalid";
    case NegotiationPayloadError::UnknownTransportKind:
      return "frame transport kind is unknown";
    case NegotiationPayloadError::UnsupportedTransportLayout:
      return "frame transport layout version is not supported";
    case NegotiationPayloadError::InvalidSlotCount:
      return "CPU frame mailbox slot count is invalid";
    case NegotiationPayloadError::InvalidCapacity:
      return "CPU frame mailbox capacity is invalid";
    case NegotiationPayloadError::ArithmeticOverflow:
      return "negotiation payload size arithmetic overflowed";
    case NegotiationPayloadError::ContractMismatch:
      return "negotiation payloads describe different transport contracts";
    case NegotiationPayloadError::NonzeroFlags:
      return "negotiation payload flags must be zero";
    case NegotiationPayloadError::NonzeroReserved:
      return "negotiation payload reserved fields must be zero";
  }
  return "unknown negotiation payload error";
}

std::uint64_t cpu_frame_mapping_capacity(
    std::uint32_t frame_capacity_bytes) noexcept {
  std::uint64_t mapping_capacity_bytes = 0;
  return calculate_cpu_frame_mapping_capacity(frame_capacity_bytes,
                                               mapping_capacity_bytes) ==
                 NegotiationPayloadError::None
             ? mapping_capacity_bytes
             : 0;
}

NegotiationPayloadError encode_open_stream_payload(
    const OpenStreamPayload& payload, std::vector<std::byte>& destination) {
  destination.clear();
  const NegotiationPayloadError validation =
      validate_open_stream_payload(payload);
  if (validation != NegotiationPayloadError::None) return validation;

  destination.assign(kOpenStreamPayloadBytes, std::byte{0});
  write_u16(destination, kNegotiationSchemaOffset, payload.schema_version);
  write_u16(destination, kNegotiationPayloadBytesOffset, payload.payload_bytes);
  write_u32(destination, kOpenStreamIdOffset, payload.stream_id);
  write_u32(destination, kOpenStreamWidthOffset, payload.width);
  write_u32(destination, kOpenStreamHeightOffset, payload.height);
  write_u32(destination, kOpenStreamFrameRateNumeratorOffset,
            payload.frame_rate_numerator);
  write_u32(destination, kOpenStreamFrameRateDenominatorOffset,
            payload.frame_rate_denominator);
  write_u32(destination, kOpenStreamPixelFormatOffset,
            static_cast<std::uint32_t>(payload.pixel_format));
  write_u32(destination, kOpenStreamPlane0StrideOffset,
            payload.plane0_stride_bytes);
  write_u32(destination, kOpenStreamPlane1StrideOffset,
            payload.plane1_stride_bytes);
  write_u32(destination, kOpenStreamFrameBytesOffset, payload.frame_bytes);
  write_u32(destination, kOpenStreamFlagsOffset, payload.flags);
  write_u32(destination, kOpenStreamReservedOffset, payload.reserved);
  return NegotiationPayloadError::None;
}

NegotiationPayloadError decode_open_stream_payload(
    std::span<const std::byte> source, OpenStreamPayload& payload) noexcept {
  payload = {};
  if (source.size() != kOpenStreamPayloadBytes) {
    return NegotiationPayloadError::WrongPayloadSize;
  }
  payload.schema_version = read_u16(source, kNegotiationSchemaOffset);
  payload.payload_bytes = read_u16(source, kNegotiationPayloadBytesOffset);
  payload.stream_id = read_u32(source, kOpenStreamIdOffset);
  payload.width = read_u32(source, kOpenStreamWidthOffset);
  payload.height = read_u32(source, kOpenStreamHeightOffset);
  payload.frame_rate_numerator =
      read_u32(source, kOpenStreamFrameRateNumeratorOffset);
  payload.frame_rate_denominator =
      read_u32(source, kOpenStreamFrameRateDenominatorOffset);
  payload.pixel_format =
      static_cast<FramePixelFormat>(read_u32(source, kOpenStreamPixelFormatOffset));
  payload.plane0_stride_bytes =
      read_u32(source, kOpenStreamPlane0StrideOffset);
  payload.plane1_stride_bytes =
      read_u32(source, kOpenStreamPlane1StrideOffset);
  payload.frame_bytes = read_u32(source, kOpenStreamFrameBytesOffset);
  payload.flags = read_u32(source, kOpenStreamFlagsOffset);
  payload.reserved = read_u32(source, kOpenStreamReservedOffset);
  return validate_open_stream_payload(payload);
}

NegotiationPayloadError encode_transport_offer_payload(
    const TransportOfferPayload& payload,
    std::vector<std::byte>& destination) {
  destination.clear();
  const NegotiationPayloadError validation =
      validate_transport_payload(payload, kTransportOfferPayloadBytes);
  if (validation != NegotiationPayloadError::None) return validation;

  destination.assign(kTransportOfferPayloadBytes, std::byte{0});
  encode_transport_fields(payload, destination);
  return NegotiationPayloadError::None;
}

NegotiationPayloadError decode_transport_offer_payload(
    std::span<const std::byte> source,
    TransportOfferPayload& payload) noexcept {
  payload = {};
  if (source.size() != kTransportOfferPayloadBytes) {
    return NegotiationPayloadError::WrongPayloadSize;
  }
  decode_transport_fields(source, payload);
  return validate_transport_payload(payload, kTransportOfferPayloadBytes);
}

NegotiationPayloadError encode_transport_descriptor_payload(
    const TransportDescriptorPayload& payload,
    std::vector<std::byte>& destination) {
  destination.clear();
  const NegotiationPayloadError validation =
      validate_transport_payload(payload, kTransportDescriptorPayloadBytes);
  if (validation != NegotiationPayloadError::None) return validation;

  destination.assign(kTransportDescriptorPayloadBytes, std::byte{0});
  encode_transport_fields(payload, destination);
  return NegotiationPayloadError::None;
}

NegotiationPayloadError decode_transport_descriptor_payload(
    std::span<const std::byte> source,
    TransportDescriptorPayload& payload) noexcept {
  payload = {};
  if (source.size() != kTransportDescriptorPayloadBytes) {
    return NegotiationPayloadError::WrongPayloadSize;
  }
  decode_transport_fields(source, payload);
  return validate_transport_payload(payload, kTransportDescriptorPayloadBytes);
}

NegotiationPayloadError validate_transport_offer_for_open_stream(
    const OpenStreamPayload& stream,
    const TransportOfferPayload& offer) noexcept {
  const NegotiationPayloadError stream_status =
      validate_open_stream_payload(stream);
  if (stream_status != NegotiationPayloadError::None) return stream_status;
  const NegotiationPayloadError offer_status =
      validate_transport_payload(offer, kTransportOfferPayloadBytes);
  if (offer_status != NegotiationPayloadError::None) return offer_status;
  // Layout 1 is deliberately a single fixed fallback contract. A capacity-only
  // comparison would accept different formats or geometries that happen to
  // contain the same number of bytes, even though the shared-memory ABI has no
  // fields with which to describe them.
  const bool fixed_contract =
      stream.width == vividcam::kCpuFrameWidth &&
      stream.height == vividcam::kCpuFrameHeight &&
      stream.frame_rate_numerator == 60U &&
      stream.frame_rate_denominator == 1U &&
      stream.pixel_format == FramePixelFormat::Nv12 &&
      stream.plane0_stride_bytes == vividcam::kCpuFrameYStrideBytes &&
      stream.plane1_stride_bytes == vividcam::kCpuFrameUvStrideBytes &&
      stream.frame_bytes == vividcam::kCpuFrameNv12Bytes &&
      offer.frame_capacity_bytes == vividcam::kCpuFrameNv12Bytes;
  return fixed_contract ? NegotiationPayloadError::None
                        : NegotiationPayloadError::ContractMismatch;
}

NegotiationPayloadError validate_transport_descriptor_for_offer(
    const TransportOfferPayload& offer,
    const TransportDescriptorPayload& descriptor) noexcept {
  const NegotiationPayloadError offer_status =
      validate_transport_payload(offer, kTransportOfferPayloadBytes);
  if (offer_status != NegotiationPayloadError::None) return offer_status;
  const NegotiationPayloadError descriptor_status =
      validate_transport_payload(descriptor,
                                 kTransportDescriptorPayloadBytes);
  if (descriptor_status != NegotiationPayloadError::None) {
    return descriptor_status;
  }
  const bool matches =
      descriptor.transport_kind == offer.transport_kind &&
      descriptor.layout_major == offer.layout_major &&
      descriptor.layout_minor == offer.layout_minor &&
      descriptor.slot_count == offer.slot_count &&
      descriptor.mapping_header_bytes == offer.mapping_header_bytes &&
      descriptor.frame_capacity_bytes == offer.frame_capacity_bytes &&
      descriptor.mapping_capacity_bytes == offer.mapping_capacity_bytes;
  return matches ? NegotiationPayloadError::None
                 : NegotiationPayloadError::ContractMismatch;
}

ProtocolError encode_header(const MessageHeader& header,
                            std::span<std::byte> destination) noexcept {
  const ProtocolError validation = validate_header(header, true);
  if (validation != ProtocolError::None) return validation;
  if (destination.size() < kHeaderBytes) {
    return ProtocolError::OutputBufferTooSmall;
  }

  std::fill_n(destination.begin(), kHeaderBytes, std::byte{0});
  for (std::size_t index = 0; index < kMagic.size(); ++index) {
    destination[kMagicOffset + index] = static_cast<std::byte>(kMagic[index]);
  }
  write_u16(destination, kHeaderBytesOffset, header.header_bytes);
  write_u16(destination, kProtocolMajorOffset, header.protocol_major);
  write_u16(destination, kProtocolMinorOffset, header.protocol_minor);
  write_u16(destination, kMessageTypeOffset,
            static_cast<std::uint16_t>(header.message_type));
  write_u32(destination, kFlagsOffset, header.flags);
  write_u32(destination, kPayloadBytesOffset, header.payload_bytes);
  write_u32(destination, kReserved0Offset, header.reserved0);
  write_u64(destination, kMessageSequenceOffset, header.message_sequence);
  write_u64(destination, kCorrelationIdOffset, header.correlation_id);
  for (std::size_t index = 0; index < header.connection_id.size(); ++index) {
    destination[kConnectionIdOffset + index] =
        static_cast<std::byte>(header.connection_id[index]);
  }
  write_u64(destination, kReserved1Offset, header.reserved1);
  return ProtocolError::None;
}

ProtocolError encode_message(const MessageHeader& header,
                             std::span<const std::byte> payload,
                             std::vector<std::byte>& destination) {
  if (payload.size() > kMaximumPayloadBytes) {
    return ProtocolError::PayloadTooLarge;
  }
  if (header.payload_bytes != payload.size()) {
    return ProtocolError::PayloadSizeMismatch;
  }
  const ProtocolError validation = validate_header(header, true);
  if (validation != ProtocolError::None) return validation;

  destination.assign(static_cast<std::size_t>(kHeaderBytes) + payload.size(),
                     std::byte{0});
  const ProtocolError encoded = encode_header(header, destination);
  if (encoded != ProtocolError::None) {
    destination.clear();
    return encoded;
  }
  std::copy(payload.begin(), payload.end(),
            destination.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
  return ProtocolError::None;
}

ProtocolError decode_header(std::span<const std::byte> source,
                            MessageHeader& header) noexcept {
  header = {};
  if (source.size() < kHeaderBytes) return ProtocolError::HeaderTruncated;
  for (std::size_t index = 0; index < kMagic.size(); ++index) {
    if (std::to_integer<std::uint8_t>(source[kMagicOffset + index]) !=
        kMagic[index]) {
      return ProtocolError::WrongMagic;
    }
  }

  header.header_bytes = read_u16(source, kHeaderBytesOffset);
  header.protocol_major = read_u16(source, kProtocolMajorOffset);
  header.protocol_minor = read_u16(source, kProtocolMinorOffset);
  header.message_type =
      static_cast<MessageType>(read_u16(source, kMessageTypeOffset));
  header.flags = read_u32(source, kFlagsOffset);
  header.payload_bytes = read_u32(source, kPayloadBytesOffset);
  header.reserved0 = read_u32(source, kReserved0Offset);
  header.message_sequence = read_u64(source, kMessageSequenceOffset);
  header.correlation_id = read_u64(source, kCorrelationIdOffset);
  for (std::size_t index = 0; index < header.connection_id.size(); ++index) {
    header.connection_id[index] =
        std::to_integer<std::uint8_t>(source[kConnectionIdOffset + index]);
  }
  header.reserved1 = read_u64(source, kReserved1Offset);
  return validate_header(header, false);
}

ProtocolError decode_message(std::span<const std::byte> source,
                             MessageView& message) noexcept {
  message = {};
  MessageHeader header;
  const ProtocolError decoded = decode_header(source, header);
  if (decoded != ProtocolError::None) return decoded;

  const std::size_t expected_bytes =
      static_cast<std::size_t>(kHeaderBytes) + header.payload_bytes;
  if (source.size() < expected_bytes) return ProtocolError::PayloadTruncated;
  if (source.size() > expected_bytes) return ProtocolError::TrailingBytes;

  message.header = header;
  message.payload = source.subspan(kHeaderBytes, header.payload_bytes);
  return ProtocolError::None;
}

} // namespace vividcam::producer_ipc
