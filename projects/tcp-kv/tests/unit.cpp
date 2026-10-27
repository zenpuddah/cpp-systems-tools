#include "tcp_kv/connection.hpp"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using namespace tcp_kv;
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expression); } while (false)

class Fake final : public Transport {
 public:
  std::string input, output;
  std::deque<IoResult> reads, writes;
  std::vector<std::string> write_arguments;
  std::size_t read_calls = 0, write_calls = 0;
  bool eof = false;
  IoResult receive(int, char* data, std::size_t size) override {
    ++read_calls;
    std::size_t amount = size;
    if (!reads.empty()) {
      const auto result = reads.front(); reads.pop_front();
      if (result.count <= 0) return result;
      amount = std::min(amount, static_cast<std::size_t>(result.count));
    }
    if (input.empty()) return eof ? IoResult{0} : IoResult{-1, EAGAIN};
    amount = std::min(amount, input.size());
    std::copy_n(input.data(), amount, data);
    input.erase(0, amount);
    return {static_cast<ssize_t>(amount)};
  }
  IoResult send(int, const char* data, std::size_t size) override {
    ++write_calls;
    write_arguments.emplace_back(data, size);
    auto result = IoResult{static_cast<ssize_t>(size)};
    if (!writes.empty()) { result = writes.front(); writes.pop_front(); }
    if (result.count > 0) {
      result.count = static_cast<ssize_t>(std::min(size, static_cast<std::size_t>(result.count)));
      output.append(data, static_cast<std::size_t>(result.count));
    }
    return result;
  }
};
const auto epoch = Clock::time_point{};
std::string response(Store& store, std::string_view line) { return store.execute(parse(line)); }
std::string repeat(std::string_view value, std::size_t count) {
  std::string result;
  while (count--) result += value;
  return result;
}
void bounded(const TurnStats& stats) {
  CHECK(stats.commands <= command_budget);
  CHECK(stats.received <= byte_budget);
  CHECK(stats.sent <= byte_budget);
  CHECK(stats.attempts <= syscall_budget);
}

void parser_and_store() {
  Store store;
  CHECK(response(store, "SET hello a  b \n") == "OK\n");
  CHECK(response(store, "GET hello\n") == "VALUE a  b \n");
  CHECK(response(store, "DEL hello\n") == "DELETED\n");
  CHECK(response(store, "DEL hello\n") == "NOT_FOUND\n");
  CHECK(response(store, "GET hello\n") == "NOT_FOUND\n");
  CHECK(response(store, "SET a  \n") == "OK\n");
  CHECK(response(store, "GET a\n") == "VALUE  \n");
  CHECK(response(store, "SET " + std::string(64, '!') + " " + std::string(1024, '~') + "\n") == "OK\n");
  const std::vector<std::pair<std::string, std::string>> cases = {
    {"", "incomplete_line"}, {"GET a", "incomplete_line"},
    {"\n", "unknown_command"}, {"get a\n", "unknown_command"},
    {"GETX a\n", "unknown_command"}, {"GET\n", "syntax"},
    {"GET \n", "syntax"}, {"GET a \n", "syntax"}, {"DEL a b\n", "syntax"},
    {"SET a\n", "syntax"}, {"SET a \n", "syntax"}, {"SET  a\n", "syntax"},
    {"GET a\r\n", "invalid_byte"}, {"SET a \t\n", "invalid_byte"},
    {std::string("SET a \x7f\n"), "invalid_byte"},
    {std::string("BAD \xff\n"), "invalid_byte"},
    {std::string("SET a \0\n", 8), "invalid_byte"},
    {"GET " + std::string(65, 'k') + "\n", "key_too_long"},
    {"SET k " + std::string(1025, 'v') + "\n", "value_too_long"},
    {"SET " + std::string(65, 'k') + " " + std::string(1025, 'v') + "\n", "key_too_long"},
    {"GET " + std::string(65, 'k') + " extra\n", "syntax"},
    {std::string(4095, 'x') + "\n", "unknown_command"},
    {std::string(4096, '\xff') + "\n", "line_too_long"},
    {"GET a\nGET b\n", "invalid_byte"},
  };
  for (const auto& [line, error] : cases) {
    const auto actual = response(store, line);
    if (actual != "ERROR " + error + "\n")
      throw std::runtime_error("parser case expected " + error + ", got " + actual);
  }
  // Printable punctuation is allowed in keys, not just alphanumerics.
  CHECK(response(store, "SET !~[]{}:;\\ value\n") == "OK\n");
  for (int byte = 0; byte <= 255; ++byte) {
    const std::string frame = "SET ascii " + std::string(1, static_cast<char>(byte)) + "\n";
    CHECK(response(store, frame) == (byte >= 0x20 && byte <= 0x7e ? "OK\n" : "ERROR invalid_byte\n"));
  }
  Store full;
  for (std::size_t i = 0; i < max_keys; ++i)
    CHECK(response(full, "SET k" + std::to_string(i) + " v\n") == "OK\n");
  CHECK(full.size() == max_keys);
  CHECK(response(full, "SET extra v\n") == "ERROR store_full\n");
  CHECK(response(full, "SET k0 replacement\n") == "OK\n");
  CHECK(response(full, "GET k0\n") == "VALUE replacement\n");
  CHECK(response(full, "DEL k1\n") == "DELETED\n");
  CHECK(response(full, "SET extra v\n") == "OK\n");
  CHECK(full.size() == max_keys);
}

void descriptor_ownership() {
  static_assert(!std::is_copy_constructible_v<Fd> && !std::is_copy_assignable_v<Fd>);
  static_assert(std::is_nothrow_move_constructible_v<Fd> && std::is_nothrow_move_assignable_v<Fd>);
  int pipefd[2];
  CHECK(::pipe(pipefd) == 0);
  {
    Fd first(pipefd[0]), second(pipefd[1]);
    Fd moved(std::move(first));
    CHECK(!first && moved.get() == pipefd[0]);
    second = std::move(moved);
    CHECK(!moved && second.get() == pipefd[0]);
    CHECK(::fcntl(pipefd[1], F_GETFD) == -1 && errno == EBADF);
    auto& same = second;
    second = std::move(same); // Self move is harmless.
    CHECK(second.get() == pipefd[0]);
  }
  CHECK(::fcntl(pipefd[0], F_GETFD) == -1 && errno == EBADF);
}

void all_split_points() {
  const std::vector<std::pair<std::string, std::string>> examples = {
    {"SET key two  spaces \n", "OK\n"}, {"GET key\n", "NOT_FOUND\n"},
    {"SET k " + std::string(1024, 'x') + "\n", "OK\n"},
    {"GET bad\r\n", "ERROR invalid_byte\n"},
    {"GET " + std::string(65, 'k') + "\n", "ERROR key_too_long\n"},
  };
  for (const auto& [frame, expected] : examples) {
    for (std::size_t split = 0; split <= frame.size(); ++split) {
      Store store; Fake io; Connection connection{Fd{}};
      io.input = frame.substr(0, split);
      bounded(connection.turn(store, io, true, true, epoch));
      CHECK(io.output == (split == frame.size() ? expected : ""));
      io.input = frame.substr(split);
      bounded(connection.turn(store, io, true, true, epoch));
      CHECK(io.output == expected);
      CHECK(connection.pending_input() == 0 && connection.pending_output() == 0);
    }
  }
}

void injected_io() {
  Store store; Fake io; Connection connection{Fd{}};
  io.input = "GET missing\n";
  io.reads = {{-1, EINTR}, {3}, {-1, EAGAIN}};
  auto stats = connection.turn(store, io, true, true, epoch);
  CHECK(stats.attempts == 3 && connection.pending_input() == 3);
  io.writes = {{2}, {-1, EINTR}, {1}, {-1, EAGAIN}};
  stats = connection.turn(store, io, true, true, epoch);
  CHECK(io.output == "NOT" && connection.pending_output() == 7);
  CHECK(io.write_arguments == std::vector<std::string>({"NOT_FOUND\n", "T_FOUND\n", "T_FOUND\n", "_FOUND\n"}));
  io.writes = {{-1, EAGAIN}};
  connection.turn(store, io, false, true, epoch);
  CHECK(io.output == "NOT" && connection.pending_output() == 7);
  connection.turn(store, io, false, true, epoch);
  CHECK(io.output == "NOT_FOUND\n" && !connection.wants_write());
  // Appending another response after a short write must compact only the
  // already-sent prefix and preserve the older response's unsent suffix.
  Fake append_io; Connection appending{Fd{}}; append_io.input = "GET missing\n";
  append_io.writes = {{2}, {-1, EAGAIN}};
  appending.turn(store, append_io, true, true, epoch);
  append_io.input = "SET a v\n";
  appending.turn(store, append_io, true, true, epoch);
  CHECK(append_io.output == "NOT_FOUND\nOK\n" && appending.pending_output() == 0);
  // Every failed syscall counts, even EINTR. It must not spin forever.
  for (std::size_t i = 0; i < 40; ++i) io.reads.push_back({-1, EINTR});
  stats = connection.turn(store, io, true, false, epoch);
  CHECK(stats.attempts == 32);
  Fake blocked_send; Connection blocked{Fd{}}; blocked_send.input = "GET missing\n";
  blocked.turn(store, blocked_send, true, false, epoch);
  for (std::size_t i = 0; i < 40; ++i) blocked_send.writes.push_back({-1, EINTR});
  stats = blocked.turn(store, blocked_send, false, true, epoch);
  CHECK(stats.attempts == 32 && blocked.pending_output() == 10);
  for (int failure : {ECONNRESET, EIO}) {
    Fake broken; Connection peer{Fd{}}; broken.reads = {{-1, failure}};
    peer.turn(store, broken, true, false, epoch); CHECK(peer.closed());
  }
  for (auto failure : {IoResult{-1, EPIPE}, IoResult{0}}) {
    Fake broken; Connection peer{Fd{}}; broken.input = "GET k\n"; broken.writes = {failure};
    peer.turn(store, broken, true, true, epoch); CHECK(peer.closed());
  }
}

void fairness_and_caps() {
  Store store; Fake io; Connection connection{Fd{}};
  io.input = repeat("GET x\n", 100);
  auto stats = connection.turn(store, io, true, true, epoch);
  bounded(stats); CHECK(stats.commands == 16 && connection.buffered_work());
  CHECK(io.input.empty());
  for (int turn = 0; turn < 6; ++turn) bounded(connection.turn(store, io, false, true, epoch));
  CHECK(io.output == repeat("NOT_FOUND\n", 100) && !connection.buffered_work());

  // Buffered work exhausts the command budget, then additional unread bytes
  // exceed the input cap by exactly one; consumed prefixes do not count.
  Fake overflow; Connection input_limited{Fd{}};
  overflow.input = repeat("GET x\n", 16) + std::string(max_input + 1, 'x');
  bounded(input_limited.turn(store, overflow, true, false, epoch));
  CHECK(input_limited.closed());
  CHECK(input_limited.pending_input() <= max_input);
  Fake exact_input; Connection input_boundary{Fd{}};
  exact_input.input = repeat("GET x\n", 16) + std::string(max_input, 'x');
  bounded(input_boundary.turn(store, exact_input, true, false, epoch));
  CHECK(!input_boundary.closed() && input_boundary.pending_input() == max_input);

  CHECK(response(store, "SET big " + std::string(1024, 'v') + "\n") == "OK\n");
  Fake slow; Connection output_limited{Fd{}};
  for (int turn = 0; turn < 4; ++turn) {
    slow.input = repeat("GET big\n", 16);
    bounded(output_limited.turn(store, slow, true, false, epoch));
    CHECK(output_limited.pending_output() <= max_output);
    if (turn < 3) CHECK(!output_limited.closed());
  }
  CHECK(output_limited.closed());
  // 63 * 1031 + 583 = 65536: the exact output boundary is legal.
  CHECK(response(store, "SET medium " + std::string(576, 'm') + "\n") == "OK\n");
  Fake exact_output; Connection output_boundary{Fd{}};
  for (int i = 0; i < 3; ++i) {
    exact_output.input = repeat("GET big\n", 16);
    output_boundary.turn(store, exact_output, true, false, epoch);
  }
  exact_output.input = repeat("GET big\n", 15) + "GET medium\n";
  output_boundary.turn(store, exact_output, true, false, epoch);
  CHECK(!output_boundary.closed() && output_boundary.pending_output() == max_output);
  exact_output.input = "GET x\n";
  output_boundary.turn(store, exact_output, true, false, epoch);
  CHECK(output_boundary.closed() && output_boundary.pending_output() == max_output);

  Fake sending; Connection sender{Fd{}};
  sending.input = repeat("GET big\n", 16);
  bounded(sender.turn(store, sending, true, false, epoch));
  CHECK(sender.pending_output() == 16 * 1031);
  stats = sender.turn(store, sending, false, true, epoch);
  CHECK(stats.sent == byte_budget && sender.pending_output() == 112);
  sender.turn(store, sending, false, true, epoch);
  CHECK(sending.output == repeat("VALUE " + std::string(1024, 'v') + "\n", 16));
  // Repeated tiny reads exercise the shared 32-call budget independently of
  // bytes and commands.
  Fake tiny; Connection receiver{Fd{}}; tiny.input = std::string(1000, 'x');
  for (int i = 0; i < 40; ++i) tiny.reads.push_back({1});
  stats = receiver.turn(store, tiny, true, false, epoch);
  CHECK(stats.received == 32 && stats.attempts == 32);
  Fake streaming; Connection bounded_receiver{Fd{}};
  streaming.input = std::string(20000, 'x');
  stats = bounded_receiver.turn(store, streaming, true, false, epoch);
  CHECK(stats.received == byte_budget && !bounded_receiver.closed());
}

void eof_recovery_deadlines() {
  Store store; Fake io; Connection peer{Fd{}};
  io.input = repeat("GET x\n", 40) + "GET tail"; io.eof = true;
  bounded(peer.turn(store, io, true, true, epoch));
  CHECK(!peer.closed());
  for (int i = 0; i < 3; ++i) bounded(peer.turn(store, io, false, true, epoch));
  CHECK(peer.closed());
  CHECK(io.output == repeat("NOT_FOUND\n", 40) + "ERROR incomplete_line\n");

  Fake long_line; Connection recovering{Fd{}};
  long_line.input = std::string(max_line, '\xff');
  recovering.turn(store, long_line, true, true, epoch);
  CHECK(long_line.output == "ERROR line_too_long\n");
  long_line.input = "tail\nSET a v\nGET a\n";
  recovering.turn(store, long_line, true, true, epoch);
  CHECK(long_line.output == "ERROR line_too_long\nOK\nVALUE v\n");
  long_line.eof = true;
  recovering.turn(store, long_line, true, true, epoch); CHECK(recovering.closed());
  Fake overlong_eof; Connection discarded_tail{Fd{}};
  overlong_eof.input = std::string(max_line + 50, 'x'); overlong_eof.eof = true;
  discarded_tail.turn(store, overlong_eof, true, true, epoch);
  CHECK(discarded_tail.closed() && overlong_eof.output == "ERROR line_too_long\n");

  Fake slow; Connection draining{Fd{}}; slow.input = "GET x\n"; slow.eof = true;
  draining.turn(store, slow, true, false, epoch);
  CHECK(!draining.closed() && !draining.wants_read() && draining.wants_write());
  draining.turn(store, slow, false, false, epoch + std::chrono::milliseconds(1999));
  CHECK(!draining.closed());
  draining.turn(store, slow, false, false, epoch + std::chrono::seconds(2));
  CHECK(draining.closed());

  Fake shutdown_io; Connection shutting_down{Fd{}}; shutdown_io.input = "GET x\nunfinished";
  shutting_down.turn(store, shutdown_io, true, false, epoch);
  shutting_down.begin_shutdown(epoch);
  CHECK(shutting_down.pending_input() == 0 && !shutting_down.wants_read());
  shutting_down.begin_shutdown(epoch + std::chrono::seconds(1)); // Never extend.
  shutting_down.turn(store, shutdown_io, false, false, epoch + std::chrono::seconds(2));
  CHECK(shutting_down.closed());

  Fake eof_first; Connection earlier_deadline{Fd{}}; eof_first.input = "GET x\n"; eof_first.eof = true;
  earlier_deadline.turn(store, eof_first, true, false, epoch);
  earlier_deadline.begin_shutdown(epoch + std::chrono::seconds(1));
  earlier_deadline.turn(store, eof_first, false, false, epoch + std::chrono::seconds(2));
  CHECK(earlier_deadline.closed());
}
}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
    {"parser/store boundaries", parser_and_store}, {"descriptor ownership", descriptor_ownership},
    {"every split point", all_split_points}, {"injected transport", injected_io},
    {"fairness and limits", fairness_and_caps}, {"EOF/recovery/deadlines", eof_recovery_deadlines},
  };
  std::size_t failures = 0;
  for (const auto& [name, test] : tests) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
  }
  std::cout << tests.size() - failures << '/' << tests.size() << " unit groups passed (release-enabled checks)\n";
  return failures ? 1 : 0;
}
