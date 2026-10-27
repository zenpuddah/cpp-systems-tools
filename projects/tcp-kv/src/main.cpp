#include "tcp_kv/connection.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
volatile std::sig_atomic_t stopped = 0;
extern "C" void on_signal(int) { stopped = 1; }
void require(bool value, const char* operation) {
  if (!value) throw std::system_error(errno, std::generic_category(), operation);
}
void configure(int fd) {
  const int status = ::fcntl(fd, F_GETFL);
  require(status != -1, "fcntl(F_GETFL)");
  require(::fcntl(fd, F_SETFL, status | O_NONBLOCK) != -1, "fcntl(O_NONBLOCK)");
  const int flags = ::fcntl(fd, F_GETFD);
  require(flags != -1, "fcntl(F_GETFD)");
  require(::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1, "fcntl(FD_CLOEXEC)");
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  require(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled) == 0,
          "setsockopt(SO_NOSIGPIPE)");
#endif
}
unsigned short port_argument(int argc, char** argv) {
  if (argc == 1) return 5555;
  if (argc != 3 || std::string_view(argv[1]) != "--port")
    throw std::invalid_argument("usage: tcp-kv [--port 0..65535]");
  unsigned int port = 0;
  const std::string_view text(argv[2]);
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
  if (error != std::errc{} || end != text.data() + text.size() || port > 65535)
    throw std::invalid_argument("port must be an integer in 0..65535");
  return static_cast<unsigned short>(port);
}
int serve(unsigned short port) {
  using namespace tcp_kv;
  struct sigaction action{};
  action.sa_handler = on_signal;
  (void)sigemptyset(&action.sa_mask); // A macro on some POSIX implementations.
  require(::sigaction(SIGINT, &action, nullptr) == 0, "sigaction(SIGINT)");
  require(::sigaction(SIGTERM, &action, nullptr) == 0, "sigaction(SIGTERM)");
  action.sa_handler = SIG_IGN;
  require(::sigaction(SIGPIPE, &action, nullptr) == 0, "sigaction(SIGPIPE)");
  Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
  require(static_cast<bool>(listener), "socket");
  configure(listener.get());
  const int enabled = 1;
  require(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled) == 0,
          "setsockopt(SO_REUSEADDR)");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  require(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "bind");
  require(::listen(listener.get(), 32) == 0, "listen");
  socklen_t length = sizeof address;
  require(::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0,
          "getsockname");
  std::cout << "READY " << ntohs(address.sin_port) << std::endl;

  Store store;
  SocketTransport transport;
  std::vector<std::unique_ptr<Connection>> clients;
  clients.reserve(max_clients);
  bool shutting_down = false;
  while (true) {
    const auto now = Clock::now();
    if (stopped && !shutting_down) {
      shutting_down = true;
      listener.reset();
      for (auto& client : clients) client->begin_shutdown(now);
    }
    std::erase_if(clients, [](const auto& client) { return client->closed(); });
    if (shutting_down && clients.empty()) return 0;
    std::vector<pollfd> descriptors;
    descriptors.push_back({listener.get(), static_cast<short>(shutting_down ? 0 : POLLIN), 0});
    bool buffered = false;
    int timeout_ms = 25;
    for (const auto& client : clients) {
      short events = 0;
      if (client->wants_read()) events |= POLLIN;
      if (client->wants_write()) events |= POLLOUT;
      descriptors.push_back({client->fd(), events, 0});
      buffered = buffered || client->buffered_work();
      if (const auto deadline = client->deadline()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count();
        timeout_ms = static_cast<int>(std::min<long long>(timeout_ms, std::max<long long>(0, remaining)));
      }
    }
    const int result = ::poll(descriptors.data(), descriptors.size(), buffered ? 0 : timeout_ms);
    if (result < 0 && errno != EINTR) require(false, "poll");
    if (result < 0 || stopped != shutting_down) continue;
    // Snapshot clients are each visited once before another bounded accept turn.
    for (std::size_t i = 0; i < clients.size(); ++i) {
      const auto events = descriptors[i + 1].revents;
      if (events & (POLLERR | POLLNVAL)) { clients[i]->close(); continue; }
      clients[i]->turn(store, transport, events & (POLLIN | POLLHUP), events & POLLOUT,
                       Clock::now());
    }
    std::erase_if(clients, [](const auto& client) { return client->closed(); });
    if (!shutting_down && (descriptors.front().revents & POLLIN)) {
      for (std::size_t attempts = 0; attempts < accept_budget; ++attempts) {
        Fd accepted(::accept(listener.get(), nullptr, nullptr));
        if (!accepted) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          // Includes transient network failures and descriptor exhaustion: do
          // not spin unboundedly or kill existing client sessions.
          break;
        }
        if (clients.size() >= max_clients) continue;
        try {
          configure(accepted.get());
          clients.push_back(std::make_unique<Connection>(std::move(accepted)));
        } catch (const std::system_error& error) {
          std::cerr << "accepted socket setup: " << error.what() << '\n';
        }
      }
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  try { return serve(port_argument(argc, argv)); }
  catch (const std::exception& error) {
    std::cerr << "tcp-kv: " << error.what() << '\n';
    return 1;
  }
}
