#include "vividcam/cpu_frame_transport.hpp"

#include <utility>

namespace vividcam {
namespace {
constexpr const char* kUnsupported =
    "VIVIDCAM CPU frame mailbox is only available on Windows";
}

class CpuFrameMailboxSource::Impl {};
class CpuFrameMailboxProducer::Impl {};

CpuFrameMailboxSource::CpuFrameMailboxSource(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CpuFrameMailboxSource::~CpuFrameMailboxSource() { close(); }

std::optional<CpuNv12Frame> CpuFrameMailboxSource::take_latest(
    std::string& error) {
  error = kUnsupported;
  return std::nullopt;
}
CpuFrameMailboxSnapshot CpuFrameMailboxSource::snapshot() const { return {}; }
std::wstring CpuFrameMailboxSource::name() const { return {}; }
bool CpuFrameMailboxSource::open() const { return false; }
void CpuFrameMailboxSource::close() noexcept {
  std::scoped_lock lock(mutex_);
  impl_.reset();
}

CpuFrameMailboxProducer::CpuFrameMailboxProducer(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
CpuFrameMailboxProducer::~CpuFrameMailboxProducer() { close(); }

bool CpuFrameMailboxProducer::publish(const CpuNv12Frame&,
                                      std::string& error) {
  error = kUnsupported;
  return false;
}
CpuFrameMailboxSnapshot CpuFrameMailboxProducer::snapshot() const { return {}; }
std::wstring CpuFrameMailboxProducer::name() const { return {}; }
bool CpuFrameMailboxProducer::open() const { return false; }
void CpuFrameMailboxProducer::close() noexcept {
  std::scoped_lock lock(mutex_);
  impl_.reset();
}

std::shared_ptr<CpuFrameMailboxSource> create_cpu_frame_mailbox_source(
    const CpuFrameMailboxOptions&, std::string& error) {
  error = kUnsupported;
  return nullptr;
}

std::shared_ptr<CpuFrameMailboxProducer> open_cpu_frame_mailbox_producer(
    const CpuFrameMailboxOptions&, std::string& error) {
  error = kUnsupported;
  return nullptr;
}

} // namespace vividcam
