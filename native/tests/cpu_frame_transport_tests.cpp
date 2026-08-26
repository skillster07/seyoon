#include "vividcam/cpu_frame_transport.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <sddl.h>
#endif

namespace {

using namespace std::chrono_literals;

constexpr std::wstring_view kRouteDigest =
    L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

vividcam::CpuFrameConnectionId connection_id(std::uint64_t seed) {
  vividcam::CpuFrameConnectionId result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    seed ^= seed << 13U;
    seed ^= seed >> 7U;
    seed ^= seed << 17U;
    result[index] = static_cast<std::uint8_t>(seed >> ((index % 8U) * 8U));
  }
  if (std::all_of(result.begin(), result.end(),
                  [](std::uint8_t value) { return value == 0; })) {
    result[0] = 1;
  }
  return result;
}

#ifdef _WIN32

std::wstring connection_id_hex(
    const vividcam::CpuFrameConnectionId& connection) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result;
  result.reserve(connection.size() * 2U);
  for (const std::uint8_t byte : connection) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

bool parse_connection_id(std::string_view encoded,
                         vividcam::CpuFrameConnectionId& connection) {
  if (encoded.size() != connection.size() * 2U) return false;
  const auto nibble = [](char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
  };
  for (std::size_t index = 0; index < connection.size(); ++index) {
    const int high = nibble(encoded[index * 2U]);
    const int low = nibble(encoded[index * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    connection[index] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return true;
}

#endif

vividcam::CpuNv12Frame patterned_frame(std::uint64_t sequence) {
  vividcam::CpuNv12Frame frame;
  frame.sequence = sequence;
  frame.timestamp_100ns = static_cast<std::int64_t>(sequence * 166'667U);
  frame.bytes.assign(vividcam::kCpuFrameNv12Bytes,
                     static_cast<std::uint8_t>(sequence & 0xffU));
  return frame;
}

#ifdef _WIN32

bool valid_pattern(const vividcam::CpuNv12Frame& frame) {
  const auto expected = static_cast<std::uint8_t>(frame.sequence & 0xffU);
  return std::all_of(frame.bytes.begin(), frame.bytes.end(),
                     [expected](std::uint8_t value) {
                       return value == expected;
                     });
}

std::wstring sid_string(PSID sid) {
  wchar_t* raw = nullptr;
  assert(IsValidSid(sid));
  assert(ConvertSidToStringSidW(sid, &raw));
  const std::wstring result(raw);
  LocalFree(raw);
  return result;
}

std::wstring current_user_sid_string() {
  HANDLE token = nullptr;
  assert(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
  DWORD bytes = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes != 0);
  std::vector<std::byte> storage(bytes);
  assert(GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes));
  CloseHandle(token);
  const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
  return sid_string(user->User.Sid);
}

std::wstring account_sid_string(const wchar_t* account) {
  DWORD sid_bytes = 0;
  DWORD domain_characters = 0;
  SID_NAME_USE sid_type = SidTypeUnknown;
  (void)LookupAccountNameW(nullptr, account, nullptr, &sid_bytes, nullptr,
                           &domain_characters, &sid_type);
  assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER && sid_bytes != 0);
  std::vector<std::byte> sid(sid_bytes);
  std::vector<wchar_t> domain(std::max<DWORD>(domain_characters, 1U), L'\0');
  DWORD sid_capacity = sid_bytes;
  DWORD domain_capacity = static_cast<DWORD>(domain.size());
  assert(LookupAccountNameW(nullptr, account, sid.data(), &sid_capacity,
                            domain.data(), &domain_capacity, &sid_type));
  return sid_string(sid.data());
}

HANDLE create_mapping_with_sddl(const std::wstring& name,
                                const std::wstring& sddl) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  assert(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.lpSecurityDescriptor = descriptor;
  HANDLE mapping = CreateFileMappingW(
      INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
      static_cast<DWORD>(vividcam::cpu_frame_mailbox_layout::kMappingBytes),
      name.c_str());
  const DWORD status = GetLastError();
  LocalFree(descriptor);
  assert(mapping != nullptr && status != ERROR_ALREADY_EXISTS);
  return mapping;
}

class RawMappingView {
 public:
  explicit RawMappingView(const std::wstring& name) {
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                name.c_str());
    if (mapping_) {
      view_ = static_cast<std::byte*>(MapViewOfFile(
          mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
          vividcam::cpu_frame_mailbox_layout::kMappingBytes));
    }
  }
  ~RawMappingView() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
  }
  RawMappingView(const RawMappingView&) = delete;
  RawMappingView& operator=(const RawMappingView&) = delete;
  [[nodiscard]] bool valid() const noexcept {
    return mapping_ != nullptr && view_ != nullptr;
  }
  [[nodiscard]] std::byte* bytes() noexcept { return view_; }

 private:
  HANDLE mapping_{nullptr};
  std::byte* view_{nullptr};
};

void write_u64(std::byte* bytes, std::size_t offset,
               std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void store_atomic(std::byte* bytes, std::size_t offset,
                  std::uint64_t value) {
  auto* field = reinterpret_cast<volatile LONG64*>(bytes + offset);
  (void)InterlockedExchange64(field, static_cast<LONG64>(value));
}

int child_main(std::string_view route_digest,
               std::string_view encoded_connection) {
  vividcam::CpuFrameConnectionId connection{};
  if (!parse_connection_id(encoded_connection, connection)) return 20;
  vividcam::CpuFrameMailboxOptions options;
  options.scope = vividcam::CpuFrameMailboxScope::NonProductionLocal;
  options.route_digest.assign(route_digest.begin(), route_digest.end());
  options.connection_id = connection;
  std::string error;
  auto producer = vividcam::open_cpu_frame_mailbox_producer(options, error);
  if (!producer) {
    std::cerr << "child open failed: " << error << '\n';
    return 21;
  }

  constexpr std::uint64_t kSynchronizedFrames = 16;
  constexpr std::uint64_t kBurstFrames = 140;
  constexpr std::uint64_t kBurstFirstSequence = 10'000;
  constexpr std::uint64_t kPublishedFrames =
      kSynchronizedFrames + kBurstFrames;
  for (std::uint64_t sequence = 1; sequence <= kSynchronizedFrames;
       ++sequence) {
    const auto frame = patterned_frame(sequence);
    if (!producer->publish(frame, error)) {
      std::cerr << "child publish failed: " << error << '\n';
      return 22;
    }
    const auto generation = producer->snapshot().published_generation;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (producer->snapshot().consumed_generation < generation &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(1ms);
    }
    if (producer->snapshot().consumed_generation < generation) {
      std::cerr << "child acknowledgement timed out\n";
      return 23;
    }
  }

  // Publish a full 1080p burst without waiting for the consumer. The parent
  // intentionally withholds take_latest() throughout this phase, exercising
  // nonblocking latest-wins behavior across a real process boundary.
  auto burst_frame = patterned_frame(kBurstFirstSequence);
  const auto burst_started = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < kBurstFrames; ++index) {
    const std::uint64_t sequence = kBurstFirstSequence + index;
    burst_frame.sequence = sequence;
    burst_frame.timestamp_100ns =
        static_cast<std::int64_t>(sequence * 166'667U);
    std::fill(burst_frame.bytes.begin(), burst_frame.bytes.end(),
              static_cast<std::uint8_t>(sequence & 0xffU));
    if (!producer->publish(burst_frame, error)) {
      std::cerr << "child burst publish failed: " << error << '\n';
      return 24;
    }
  }
  const auto burst_duration =
      std::chrono::steady_clock::now() - burst_started;
  if (burst_duration > 5s) {
    std::cerr << "child burst publish exceeded its bounded completion budget\n";
    return 25;
  }

  const auto final_generation = producer->snapshot().published_generation;
  const auto final_deadline = std::chrono::steady_clock::now() + 8s;
  while (producer->snapshot().consumed_generation < final_generation &&
         std::chrono::steady_clock::now() < final_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  if (producer->snapshot().consumed_generation < final_generation) {
    std::cerr << "child final burst acknowledgement timed out\n";
    return 26;
  }

  const auto statistics = producer->snapshot();
  if (statistics.published_frames != kPublishedFrames ||
      statistics.consumed_frames != kSynchronizedFrames + 1U ||
      statistics.overwritten_frames == 0 || statistics.torn_reads != 0 ||
      statistics.invalid_frames != 0) {
    return 27;
  }
  return 0;
}

void run_cross_process_test() {
  const auto seed = static_cast<std::uint64_t>(GetCurrentProcessId()) ^
                    static_cast<std::uint64_t>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count());
  vividcam::CpuFrameMailboxOptions options;
  options.scope = vividcam::CpuFrameMailboxScope::NonProductionLocal;
  options.route_digest.assign(kRouteDigest);
  options.connection_id = connection_id(seed);
  std::string error;
  auto source = vividcam::create_cpu_frame_mailbox_source(options, error);
  assert(source && error.empty());

  std::vector<wchar_t> executable(32768, L'\0');
  const DWORD executable_length = GetModuleFileNameW(
      nullptr, executable.data(), static_cast<DWORD>(executable.size()));
  assert(executable_length > 0 && executable_length < executable.size());
  std::wstring command = L"\"" +
                         std::wstring(executable.data(), executable_length) +
                         L"\" --cpu-frame-mailbox-child ";
  command.append(kRouteDigest);
  command.push_back(L' ');
  command += connection_id_hex(options.connection_id);
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  assert(CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
  CloseHandle(process.hThread);

  constexpr std::uint64_t kSynchronizedFrames = 16;
  constexpr std::uint64_t kBurstFrames = 140;
  constexpr std::uint64_t kBurstFirstSequence = 10'000;
  constexpr std::uint64_t kFinalSequence =
      kBurstFirstSequence + kBurstFrames - 1U;
  constexpr std::uint64_t kPublishedFrames =
      kSynchronizedFrames + kBurstFrames;
  std::uint64_t synchronized_frames = 0;
  std::uint64_t last_sequence = 0;
  bool burst_published = false;
  bool final_frame_observed = false;
  bool second_writer_rejected = false;
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  bool child_exited = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!second_writer_rejected &&
        source->snapshot().published_generation != 0) {
      auto second_writer =
          vividcam::open_cpu_frame_mailbox_producer(options, error);
      assert(!second_writer);
      assert(error.find("already has a producer writer") != std::string::npos);
      second_writer_rejected = true;
    }
    if (synchronized_frames < kSynchronizedFrames) {
      if (const auto frame = source->take_latest(error)) {
        assert(error.empty());
        assert(frame->sequence == last_sequence + 1U);
        assert(frame->valid());
        assert(valid_pattern(*frame));
        last_sequence = frame->sequence;
        ++synchronized_frames;
      }
    } else if (!burst_published) {
      // Deliberately do not consume while the child publishes its entire
      // burst. Polling metadata only makes the slow-consumer condition
      // deterministic without introducing a test-only acknowledgement.
      const auto statistics = source->snapshot();
      burst_published =
          statistics.published_generation == kPublishedFrames;
    } else if (!final_frame_observed) {
      const auto frame = source->take_latest(error);
      assert(frame);
      assert(error.empty());
      assert(frame->sequence == kFinalSequence);
      assert(frame->valid());
      assert(valid_pattern(*frame));
      final_frame_observed = true;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, 0);
    assert(wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT);
    if (wait == WAIT_OBJECT_0) {
      child_exited = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  assert(child_exited);
  DWORD exit_code = 0;
  assert(GetExitCodeProcess(process.hProcess, &exit_code));
  CloseHandle(process.hProcess);
  assert(exit_code == 0);
  assert(synchronized_frames == kSynchronizedFrames);
  assert(burst_published);
  assert(final_frame_observed);
  assert(second_writer_rejected);
  const auto statistics = source->snapshot();
  assert(statistics.published_frames == kPublishedFrames);
  assert(statistics.consumed_frames == kSynchronizedFrames + 1U);
  assert(statistics.overwritten_frames > 0);
  assert(statistics.torn_reads == 0);
  assert(statistics.invalid_frames == 0);

  // A normal producer teardown releases the single-writer claim. A crashed
  // producer is handled by negotiating a fresh connection ID and mapping.
  auto replacement = vividcam::open_cpu_frame_mailbox_producer(options, error);
  assert(replacement && error.empty());
  replacement->close();
}

void run_production_security_regression_tests() {
  const auto seed = static_cast<std::uint64_t>(GetCurrentProcessId()) ^
                    static_cast<std::uint64_t>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count());
  vividcam::CpuFrameMailboxOptions options;
  options.scope =
      vividcam::CpuFrameMailboxScope::ProductionSecurityLocalTest;
  options.route_digest.assign(kRouteDigest);
  options.connection_id = connection_id(seed);
  options.producer_user_sid = current_user_sid_string();

  std::string error;
  auto source = vividcam::create_cpu_frame_mailbox_source(options, error);
  assert(source && error.empty());
  assert(source->name().starts_with(L"Local\\VIVIDCAM.Frame.v1."));
  auto producer = vividcam::open_cpu_frame_mailbox_producer(options, error);
  assert(producer && error.empty());
  assert(producer->publish(patterned_frame(1), error));
  const auto frame = source->take_latest(error);
  assert(frame && frame->sequence == 1 && valid_pattern(*frame));
  producer->close();
  source->close();

  const std::wstring frame_server_sid =
      account_sid_string(L"NT SERVICE\\FrameServer");
  const std::wstring exact_dacl =
      L"D:P(A;;GA;;;SY)(A;;GA;;;" + frame_server_sid +
      L")(A;;GRGW;;;" + options.producer_user_sid + L")";

  options.connection_id = connection_id(seed ^ 0xb04dadacULL);
  std::wstring name;
  assert(vividcam::make_cpu_frame_mailbox_name(options, name, error));
  HANDLE mapping = create_mapping_with_sddl(
      name, exact_dacl + L"(A;;GR;;;WD)S:(ML;;NW;;;ME)");
  auto broad_acl = vividcam::open_cpu_frame_mailbox_producer(options, error);
  assert(!broad_acl && error.find("exactly three") != std::string::npos);
  CloseHandle(mapping);

  options.connection_id = connection_id(seed ^ 0x10abe1ULL);
  assert(vividcam::make_cpu_frame_mailbox_name(options, name, error));
  mapping = create_mapping_with_sddl(name,
                                     exact_dacl + L"S:(ML;;NW;;;LW)");
  auto wrong_label = vividcam::open_cpu_frame_mailbox_producer(options, error);
  assert(!wrong_label &&
         error.find("not exact Medium/no-write-up") != std::string::npos);
  CloseHandle(mapping);
}

void run_windows_counter_tests() {
  vividcam::CpuFrameMailboxOptions options;
  options.scope = vividcam::CpuFrameMailboxScope::NonProductionLocal;
  options.route_digest.assign(kRouteDigest);
  options.connection_id = connection_id(
      static_cast<std::uint64_t>(GetCurrentProcessId()) ^ 0xa51ce55ULL);
  std::string error;
  auto source = vividcam::create_cpu_frame_mailbox_source(options, error);
  assert(source && error.empty());
  auto producer = vividcam::open_cpu_frame_mailbox_producer(options, error);
  assert(producer && error.empty());
  assert(source->name() == producer->name());
  assert(source->snapshot().open && producer->snapshot().open);

  auto invalid = patterned_frame(1);
  invalid.bytes.pop_back();
  assert(!producer->publish(invalid, error));
  assert(!error.empty());
  assert(source->snapshot().invalid_frames == 1);

  assert(producer->publish(patterned_frame(1), error));
  const auto first = source->take_latest(error);
  assert(first && first->sequence == 1 && valid_pattern(*first));
  assert(!source->take_latest(error));
  assert(error.empty());

  assert(producer->publish(patterned_frame(2), error));
  assert(producer->publish(patterned_frame(3), error));
  assert(source->snapshot().overwritten_frames == 1);
  const auto newest = source->take_latest(error);
  assert(newest && newest->sequence == 3 && valid_pattern(*newest));

  RawMappingView raw(source->name());
  assert(raw.valid());
  assert(producer->publish(patterned_frame(4), error));
  auto generation = producer->snapshot().published_generation;
  auto slot = vividcam::cpu_frame_mailbox_layout::slot_offset(
      static_cast<std::size_t>(
          (generation - 1U) % vividcam::kCpuFrameMailboxSlotCount));
  store_atomic(raw.bytes(),
               slot + vividcam::cpu_frame_mailbox_layout::
                          kSlotBeginGenerationOffset,
               0);
  assert(!source->take_latest(error));
  assert(source->snapshot().torn_reads == 1);
  store_atomic(raw.bytes(),
               slot + vividcam::cpu_frame_mailbox_layout::
                          kSlotBeginGenerationOffset,
               generation);
  const auto recovered_torn = source->take_latest(error);
  assert(recovered_torn && recovered_torn->sequence == 4);

  assert(producer->publish(patterned_frame(5), error));
  generation = producer->snapshot().published_generation;
  slot = vividcam::cpu_frame_mailbox_layout::slot_offset(
      static_cast<std::size_t>(
          (generation - 1U) % vividcam::kCpuFrameMailboxSlotCount));
  write_u64(raw.bytes(),
            slot + vividcam::cpu_frame_mailbox_layout::
                       kSlotPayloadBytesOffset,
            1);
  assert(!source->take_latest(error));
  assert(!error.empty());
  assert(source->snapshot().invalid_frames == 2);
  write_u64(raw.bytes(),
            slot + vividcam::cpu_frame_mailbox_layout::
                       kSlotPayloadBytesOffset,
            vividcam::kCpuFrameNv12Bytes);
  const auto recovered_invalid = source->take_latest(error);
  assert(recovered_invalid && recovered_invalid->sequence == 5);

  producer->close();
  assert(!producer->open());
  assert(!producer->publish(patterned_frame(6), error));
  source->close();
  assert(!source->open());
  assert(!source->snapshot().open);
}

#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  if (argc == 4 && std::string_view(argv[1]) ==
                       "--cpu-frame-mailbox-child") {
    return child_main(argv[2], argv[3]);
  }
#else
  (void)argc;
  (void)argv;
#endif

  static_assert(vividcam::kCpuFrameNv12Bytes == 3'110'400);
  static_assert(vividcam::cpu_frame_mailbox_layout::kMappingBytes ==
                6'230'016);
  std::string error;
  std::wstring name;
  vividcam::CpuFrameMailboxOptions options;
  options.route_digest.assign(kRouteDigest);
  options.connection_id = connection_id(0x123456789abcdef0ULL);
  assert(vividcam::make_cpu_frame_mailbox_name(options, name, error));
  assert(name.starts_with(L"Local\\VIVIDCAM.Frame.v1."));
  assert(name.find(kRouteDigest) != std::wstring::npos);
  options.scope = vividcam::CpuFrameMailboxScope::ProductionGlobal;
  assert(vividcam::make_cpu_frame_mailbox_name(options, name, error));
  assert(name.starts_with(L"Global\\VIVIDCAM.Frame.v1."));

  options.route_digest = L"not-a-digest";
  assert(!vividcam::make_cpu_frame_mailbox_name(options, name, error));
  options.route_digest.assign(kRouteDigest);
  options.connection_id = {};
  assert(!vividcam::make_cpu_frame_mailbox_name(options, name, error));
  options.connection_id = connection_id(1);
  options.scope = static_cast<vividcam::CpuFrameMailboxScope>(0xffffU);
  assert(!vividcam::make_cpu_frame_mailbox_name(options, name, error));

  auto valid = patterned_frame(1);
  assert(valid.valid());
  valid.sequence = 0;
  assert(!valid.valid());
  valid = patterned_frame(1);
  valid.timestamp_100ns = -1;
  assert(!valid.valid());
  valid = patterned_frame(1);
  valid.y_stride_bytes += 1;
  assert(!valid.valid());

#ifdef _WIN32
  run_production_security_regression_tests();
  run_windows_counter_tests();
  run_cross_process_test();
#else
  options.scope = vividcam::CpuFrameMailboxScope::NonProductionLocal;
  options.connection_id = connection_id(1);
  assert(!vividcam::create_cpu_frame_mailbox_source(options, error));
  assert(!error.empty());
  assert(!vividcam::open_cpu_frame_mailbox_producer(options, error));
  assert(!error.empty());
#endif

  std::cout << "VIVIDCAM CPU frame mailbox tests passed\n";
  return 0;
}
