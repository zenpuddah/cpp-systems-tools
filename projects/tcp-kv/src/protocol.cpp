#include "tcp_kv/protocol.hpp"

#include <utility>

namespace tcp_kv {
namespace {
Request error(std::string reason) {
  return {Command::invalid, {}, {}, std::move(reason)};
}
}  // namespace

Request parse(std::string_view frame) {
  if (frame.size() > max_line) return error("line_too_long");
  if (frame.empty() || frame.back() != '\n') return error("incomplete_line");
  frame.remove_suffix(1);
  for (unsigned char c : frame) {
    if (c < 0x20 || c > 0x7e) return error("invalid_byte");
  }
  const auto separator = frame.find(' ');
  const auto verb = frame.substr(0, separator);
  Command command;
  if (verb == "SET") command = Command::set;
  else if (verb == "GET") command = Command::get;
  else if (verb == "DEL") command = Command::del;
  else return error("unknown_command");
  if (separator == std::string_view::npos) return error("syntax");
  auto rest = frame.substr(separator + 1);
  std::string_view key;
  std::string_view value;
  if (command == Command::set) {
    const auto split = rest.find(' ');
    if (split == std::string_view::npos) return error("syntax");
    key = rest.substr(0, split);
    value = rest.substr(split + 1);
    if (key.empty() || value.empty()) return error("syntax");
  } else {
    if (rest.empty() || rest.find(' ') != std::string_view::npos) return error("syntax");
    key = rest;
  }
  if (key.size() > 64) return error("key_too_long");
  if (value.size() > 1024) return error("value_too_long");
  return {command, std::string(key), std::string(value), {}};
}

std::string Store::execute(const Request& request) {
  if (!request.error.empty()) return "ERROR " + request.error + "\n";
  switch (request.command) {
    case Command::set: {
      const auto found = values_.find(request.key);
      if (found == values_.end() && values_.size() == max_keys) return "ERROR store_full\n";
      values_.insert_or_assign(request.key, request.value);
      return "OK\n";
    }
    case Command::get: {
      const auto found = values_.find(request.key);
      return found == values_.end() ? "NOT_FOUND\n" : "VALUE " + found->second + "\n";
    }
    case Command::del:
      return values_.erase(request.key) ? "DELETED\n" : "NOT_FOUND\n";
    case Command::invalid:
      return "ERROR syntax\n";
  }
  return "ERROR syntax\n";
}
}  // namespace tcp_kv
