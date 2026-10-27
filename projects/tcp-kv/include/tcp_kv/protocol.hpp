#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tcp_kv {
inline constexpr std::size_t max_line = 4096;
inline constexpr std::size_t max_keys = 1024;
inline constexpr std::size_t max_clients = 32;
inline constexpr std::size_t max_input = 8 * 1024;
inline constexpr std::size_t max_output = 64 * 1024;
inline constexpr std::size_t command_budget = 16;
inline constexpr std::size_t byte_budget = 16 * 1024;
inline constexpr std::size_t syscall_budget = 32;
inline constexpr std::size_t accept_budget = 8;

enum class Command { set, get, del, invalid };
struct Request {
  Command command = Command::invalid;
  std::string key;
  std::string value;
  std::string error;
};
// Pure: accepts exactly one complete LF-terminated frame, including its LF.
Request parse(std::string_view frame);

class Store {
 public:
  std::string execute(const Request& request);
  std::size_t size() const noexcept { return values_.size(); }
 private:
  std::unordered_map<std::string, std::string> values_;
};
}  // namespace tcp_kv
