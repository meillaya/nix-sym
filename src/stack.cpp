#include "stack.hpp"

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nix_sym {
namespace {

std::string trim(const std::string& value) {
  size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

bool parse_address(const std::string& token, std::uint64_t* address) {
  if (token.empty()) {
    return false;
  }
  size_t consumed = 0;
  try {
    unsigned long long parsed = std::stoull(token, &consumed, 0);
    if (consumed != token.size()) {
      return false;
    }
    *address = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool looks_like_build_id(const std::string& value) {
  if (value.size() < 8 || (value.size() % 2) != 0) {
    return false;
  }
  for (unsigned char c : value) {
    if (!std::isxdigit(c)) {
      return false;
    }
  }
  return true;
}

} // namespace

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

  std::vector<StackRecord> records;
  std::string previous_module;
  std::string line;
  std::size_t number = 0;
  while (std::getline(*input, line)) {
    ++number;
    auto comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    std::istringstream stream(line);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) {
      tokens.push_back(token);
    }
    if (tokens.empty() || tokens.size() > 3) {
      throw std::runtime_error("stack file line " + std::to_string(number) +
                               ": expected MODULE ADDRESS [BUILD-ID]");
    }

    StackRecord record;
    record.line = number;
    if (tokens.size() == 1) {
      if (previous_module.empty()) {
        throw std::runtime_error("stack file line " + std::to_string(number) +
                                 ": address without a module");
      }
      record.module = previous_module;
      if (!parse_address(tokens[0], &record.address)) {
        throw std::runtime_error("stack file line " + std::to_string(number) +
                                 ": invalid address " + tokens[0]);
      }
    } else {
      record.module = tokens[0];
      if (!parse_address(tokens[1], &record.address)) {
        throw std::runtime_error("stack file line " + std::to_string(number) +
                                 ": invalid address " + tokens[1]);
      }
      if (tokens.size() == 3) {
        if (!looks_like_build_id(tokens[2])) {
          throw std::runtime_error("stack file line " + std::to_string(number) +
                                   ": invalid build id " + tokens[2]);
        }
        record.build_id = tokens[2];
      }
      previous_module = record.module;
    }
    records.push_back(std::move(record));
  }
  return records;
}

} // namespace nix_sym
