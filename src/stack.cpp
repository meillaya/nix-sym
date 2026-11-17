#include "stack.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nix_sym {
namespace {

constexpr std::size_t kMinBuildIdLength = 8;

std::string_view trim(std::string_view value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string_view> split_tokens(std::string_view line) {
  std::vector<std::string_view> tokens;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) != 0) {
      ++index;
    }
    const std::size_t begin = index;
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) == 0) {
      ++index;
    }
    if (index > begin) {
      tokens.push_back(line.substr(begin, index - begin));
    }
  }
  return tokens;
}

// C integer syntax with base 0 ("0x" hexadecimal). Unlike std::stoull this
// rejects a leading '+' or '-': an address is never negative.
bool parse_number(std::string_view token, std::uint64_t* value) {
  if (token.empty() || std::isdigit(static_cast<unsigned char>(token.front())) == 0) {
    return false;
  }
  const std::string text(token);
  std::size_t consumed = 0;
  try {
    const unsigned long long parsed = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) {
      return false;
    }
    *value = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// A build id is an even-length hexadecimal string of at least 8 characters.
// Paths contain '/' or other non-hex bytes, so they cannot match.
bool looks_like_build_id(std::string_view value) {
  if (value.size() < kMinBuildIdLength || (value.size() % 2) != 0) {
    return false;
  }
  for (const unsigned char c : value) {
    if (std::isxdigit(c) == 0) {
      return false;
    }
  }
  return true;
}

std::string quoted(std::string_view token) {
  std::string result;
  result.reserve(token.size() + 2);
  result.push_back('\'');
  result += token;
  result.push_back('\'');
  return result;
}

[[noreturn]] void fail(std::size_t line_number, const std::string& message) {
  throw std::runtime_error("stack file line " + std::to_string(line_number) + ": " + message);
}

void parse_record(std::string_view line, std::size_t number, std::vector<StackRecord>* records,
                  std::string* previous_module, std::string* previous_build_id) {
  const std::vector<std::string_view> tokens = split_tokens(line);
  if (tokens.empty()) {
    return;
  }
  if (tokens.size() > 4) {
    fail(number, "expected MODULE ADDRESS [BUILD-ID] or one of the runtime/build-id forms, got " +
                     std::to_string(tokens.size()) + " tokens");
  }

  StackRecord record;
  record.line = number;
  const bool build_id_first = looks_like_build_id(tokens[0]);

  if (tokens.size() == 1) {
    if (previous_module->empty() && previous_build_id->empty()) {
      fail(number, "address without a module");
    }
    if (!parse_number(tokens[0], &record.address)) {
      fail(number, "invalid address " + quoted(tokens[0]));
    }
    record.module = *previous_module;
    record.build_id = *previous_build_id;
    records->push_back(std::move(record));
    return;
  }

  // Two forms carry an optional "@ LOAD_BASE" suffix:
  //   MODULE RUNTIME_ADDRESS @ LOAD_BASE
  //   BUILD-ID RUNTIME_ADDRESS @ LOAD_BASE
  if (tokens.size() == 4) {
    if (tokens[2] != "@") {
      fail(number, "expected '@' before the load base, got " + quoted(tokens[2]));
    }
    if (!parse_number(tokens[3], &record.load_base)) {
      fail(number, "invalid load base " + quoted(tokens[3]));
    }
    record.runtime_address = true;
  } else if (tokens.size() == 3 && tokens[1] == "@") {
    fail(number, "missing load base after '@'");
  } else if (tokens.size() == 2 && tokens[1] == "@") {
    fail(number, "missing load base after '@'");
  }

  if (!parse_number(tokens[1], &record.address)) {
    fail(number, "invalid address " + quoted(tokens[1]));
  }

  if (build_id_first) {
    // BUILD-ID ADDRESS [@ LOAD_BASE]: the module path is resolved later from
    // the executable that debuginfod returns for this build id.
    if (tokens.size() == 3) {
      fail(number, "a build-id record accepts at most BUILD-ID ADDRESS [@ LOAD_BASE]");
    }
    record.build_id = std::string(tokens[0]);
    record.module.clear();
    *previous_module = record.module;
    *previous_build_id = record.build_id;
  } else {
    // MODULE ADDRESS [BUILD-ID] and MODULE RUNTIME_ADDRESS @ LOAD_BASE.
    record.module = std::string(tokens[0]);
    if (tokens.size() == 3) {
      if (!looks_like_build_id(tokens[2])) {
        fail(number, "invalid build id " + quoted(tokens[2]));
      }
      record.build_id = std::string(tokens[2]);
    }
    *previous_module = record.module;
    previous_build_id->clear();
  }

  if (record.runtime_address && record.address < record.load_base) {
    fail(number,
         "runtime address " + quoted(tokens[1]) + " is below load base " + quoted(tokens[3]));
  }
  records->push_back(std::move(record));
}

} // namespace

std::vector<StackRecord> parse_stack_text(std::string_view text) {
  std::vector<StackRecord> records;
  std::string previous_module;
  std::string previous_build_id;

  std::size_t number = 0;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t newline = text.find('\n', begin);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    std::string_view line = text.substr(begin, end - begin);
    ++number;

    const std::size_t comment = line.find('#');
    if (comment != std::string_view::npos) {
      line = line.substr(0, comment);
    }
    line = trim(line);
    if (!line.empty()) {
      parse_record(line, number, &records, &previous_module, &previous_build_id);
    }

    if (newline == std::string_view::npos) {
      break;
    }
    begin = newline + 1;
  }
  return records;
}

std::vector<StackRecord> read_stack_file(const std::string& path) {
  std::ifstream file;
  std::istream* input = nullptr;
  if (path == "-") {
    input = &std::cin;
  } else {
    file.open(path);
    if (!file) {
      throw std::runtime_error("cannot open stack file: " + path);
    }
    input = &file;
  }

  std::ostringstream buffer;
  buffer << input->rdbuf();
  if (input->bad()) {
    throw std::runtime_error("cannot read stack file: " + path);
  }
  return parse_stack_text(buffer.str());
}

bool stack_record_lookup_address(const StackRecord& record, std::uint64_t* lookup_address) {
  if (lookup_address == nullptr) {
    return false;
  }
  if (!record.runtime_address) {
    *lookup_address = record.address;
    return true;
  }
  if (record.address < record.load_base) {
    return false;
  }
  *lookup_address = record.address - record.load_base;
  return true;
}

} // namespace nix_sym
