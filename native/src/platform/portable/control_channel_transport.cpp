#include "vividcam/control_channel_transport.hpp"

#include <mutex>
#include <utility>

namespace vividcam {
namespace {

constexpr const char* kUnsupported =
    "VIVIDCAM control transport is only available on Windows";

} // namespace

class ProducerControlServer::Impl {
 public:
  bool start(std::wstring, std::string, std::string& error) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::scoped_lock state_lock(mutex_);
    snapshot_.last_error = kUnsupported;
    error = kUnsupported;
    return false;
  }

  void stop() noexcept {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::scoped_lock state_lock(mutex_);
    snapshot_.running = false;
    snapshot_.connected = false;
  }

  ControlChannelTransportSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
  }

 private:
  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  ControlChannelTransportSnapshot snapshot_;
};

class SourceControlClient::Impl {
 public:
  bool start(std::wstring, std::string& error) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::scoped_lock state_lock(mutex_);
    snapshot_.last_error = kUnsupported;
    error = kUnsupported;
    return false;
  }

  void stop() noexcept {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    std::scoped_lock state_lock(mutex_);
    snapshot_.running = false;
    snapshot_.connected = false;
  }

  ControlChannelTransportSnapshot snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
  }

 private:
  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  ControlChannelTransportSnapshot snapshot_;
};

bool find_registered_vividcam_control_route(std::wstring& route,
                                            std::string& error) {
  route.clear();
  error = kUnsupported;
  return false;
}

bool make_vividcam_control_pipe_name(std::wstring_view,
                                     std::wstring& pipe_name,
                                     std::string& error) {
  pipe_name.clear();
  error = kUnsupported;
  return false;
}

ProducerControlServer::ProducerControlServer() : impl_(std::make_unique<Impl>()) {}
ProducerControlServer::~ProducerControlServer() = default;

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
SourceControlClient::~SourceControlClient() = default;

bool SourceControlClient::start(std::wstring route, std::string& error) {
  return impl_->start(std::move(route), error);
}

void SourceControlClient::stop() noexcept { impl_->stop(); }

ControlChannelTransportSnapshot SourceControlClient::snapshot() const {
  return impl_->snapshot();
}

} // namespace vividcam
