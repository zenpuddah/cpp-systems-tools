#include "tcp_kv/connection.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/socket.h>

namespace tcp_kv {
IoResult SocketTransport::receive(int fd, char* data, std::size_t size) {
  const auto n = ::recv(fd, data, size, 0);
  return {n, n < 0 ? errno : 0};
}
IoResult SocketTransport::send(int fd, const char* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
  const auto n = ::send(fd, data, size, MSG_NOSIGNAL);
#else
  const auto n = ::send(fd, data, size, 0);
#endif
  return {n, n < 0 ? errno : 0};
}

bool Connection::buffered_work() const {
  if (closed_ || shutdown_) return false;
  return input_.find('\n') != std::string::npos || input_.size() >= max_line ||
         (eof_ && !input_.empty());
}

bool Connection::enqueue(std::string response) {
  if (response.size() > max_output - pending_output()) {
    close();
    return false;
  }
  // Compact only consumed bytes; the public limit always counts unsent bytes.
  if (output_offset_ != 0) {
    output_.erase(0, output_offset_);
    output_offset_ = 0;
  }
  output_ += response;
  return true;
}

void Connection::process(Store& store, TurnStats& stats) {
  while (!closed_ && stats.commands < command_budget) {
    const auto lf = input_.find('\n');
    if (discarding_) {
      if (lf == std::string::npos) { input_.clear(); return; }
      input_.erase(0, lf + 1);
      discarding_ = false;
      continue;
    }
    if (lf != std::string::npos) {
      const auto response = store.execute(parse(std::string_view(input_).substr(0, lf + 1)));
      input_.erase(0, lf + 1);
      ++stats.commands;
      if (!enqueue(response)) return;
    } else if (input_.size() >= max_line) {
      ++stats.commands;
      input_.clear();
      discarding_ = true;
      if (!enqueue("ERROR line_too_long\n")) return;
    } else if (eof_ && !input_.empty()) {
      ++stats.commands;
      input_.clear();
      if (!enqueue("ERROR incomplete_line\n")) return;
    } else return;
  }
}

void Connection::begin_shutdown(Clock::time_point now) {
  if (closed_ || shutdown_) return;
  shutdown_ = true;
  input_.clear();
  const auto limit = now + std::chrono::seconds(2);
  if (!deadline_set_ || limit < deadline_) deadline_ = limit;
  deadline_set_ = true;
  finish(now);
}

void Connection::finish(Clock::time_point now) {
  if (closed_) return;
  if (deadline_set_ && now >= deadline_) { close(); return; }
  if ((shutdown_ || (eof_ && input_.empty())) && pending_output() == 0) close();
}

TurnStats Connection::turn(Store& store, Transport& io, bool readable, bool writable,
                           Clock::time_point now) {
  TurnStats stats;
  finish(now);
  if (closed_) return stats;
  if (!shutdown_) process(store, stats);
  std::array<char, 4096> buffer{};
  while (!closed_ && wants_read() && readable && stats.received < byte_budget &&
         stats.attempts < syscall_budget) {
    const auto room = max_input - input_.size();
    const auto amount = std::min({buffer.size(), byte_budget - stats.received, room + 1});
    ++stats.attempts;
    const auto result = io.receive(fd(), buffer.data(), amount);
    if (result.count > 0) {
      const auto count = static_cast<std::size_t>(result.count);
      stats.received += count;
      if (count > room) { close(); break; }
      input_.append(buffer.data(), count);
      process(store, stats);
    } else if (result.count == 0) {
      eof_ = true;
      deadline_set_ = true;
      deadline_ = now + std::chrono::seconds(2);
      process(store, stats);
      break;
    } else if (result.error == EINTR) continue;
    else if (result.error == EAGAIN || result.error == EWOULDBLOCK) break;
    else { close(); break; }
  }
  while (!closed_ && writable && pending_output() != 0 && stats.sent < byte_budget &&
         stats.attempts < syscall_budget) {
    const auto amount = std::min(pending_output(), byte_budget - stats.sent);
    ++stats.attempts;
    const auto result = io.send(fd(), output_.data() + output_offset_, amount);
    if (result.count > 0) {
      const auto count = static_cast<std::size_t>(result.count);
      output_offset_ += count;
      stats.sent += count;
      if (pending_output() == 0) { output_.clear(); output_offset_ = 0; }
    } else if (result.count < 0 && result.error == EINTR) continue;
    else if (result.count < 0 && (result.error == EAGAIN || result.error == EWOULDBLOCK)) break;
    else { close(); break; }
  }
  finish(now);
  return stats;
}
}  // namespace tcp_kv
