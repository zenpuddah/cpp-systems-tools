#pragma once

#include "tcp_kv/fd.hpp"
#include "tcp_kv/protocol.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <sys/types.h>

namespace tcp_kv {
using Clock = std::chrono::steady_clock;
struct IoResult { ssize_t count; int error = 0; };
class Transport {
 public:
  virtual ~Transport() = default;
  virtual IoResult receive(int fd, char* data, std::size_t size) = 0;
  virtual IoResult send(int fd, const char* data, std::size_t size) = 0;
};
class SocketTransport final : public Transport {
 public:
  IoResult receive(int fd, char* data, std::size_t size) override;
  IoResult send(int fd, const char* data, std::size_t size) override;
};
struct TurnStats {
  std::size_t commands = 0, received = 0, sent = 0, attempts = 0;
};

class Connection {
 public:
  explicit Connection(Fd fd) : fd_(std::move(fd)) {}
  int fd() const noexcept { return fd_.get(); }
  bool closed() const noexcept { return closed_; }
  bool wants_read() const noexcept { return !closed_ && !eof_ && !shutdown_; }
  bool wants_write() const noexcept { return !closed_ && pending_output() != 0; }
  bool buffered_work() const;
  std::size_t pending_input() const noexcept { return input_.size(); }
  std::size_t pending_output() const noexcept { return output_.size() - output_offset_; }
  std::optional<Clock::time_point> deadline() const noexcept {
    return deadline_set_ ? std::optional{deadline_} : std::nullopt;
  }
  void begin_shutdown(Clock::time_point now);
  void close() noexcept { closed_ = true; fd_.reset(); }
  TurnStats turn(Store&, Transport&, bool readable, bool writable, Clock::time_point now);
 private:
  bool enqueue(std::string response);
  void process(Store&, TurnStats&);
  void finish(Clock::time_point now);
  Fd fd_;
  std::string input_, output_;
  std::size_t output_offset_ = 0;
  bool closed_ = false, eof_ = false, shutdown_ = false, discarding_ = false;
  bool deadline_set_ = false;
  Clock::time_point deadline_{};
};
}  // namespace tcp_kv
