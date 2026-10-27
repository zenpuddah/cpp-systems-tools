#pragma once

#include <unistd.h>
#include <utility>

namespace tcp_kv {
class Fd {
 public:
  explicit Fd(int fd = -1) noexcept : fd_(fd) {}
  ~Fd() { reset(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : fd_(other.release()) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }
  int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int replacement = -1) noexcept {
    // Never retry close(EINTR): the number may already have been reused.
    if (fd_ >= 0) ::close(fd_);
    fd_ = replacement;
  }
 private:
  int fd_;
};
}  // namespace tcp_kv
