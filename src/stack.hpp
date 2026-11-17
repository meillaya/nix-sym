#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nix_sym {

struct StackRecord {
  std::string module;
  std::string build_id;
  std::uint64_t address = 0;
  std::size_t line = 0;
};

// Read a stack file. Each non-comment line is:
//   MODULE ADDRESS [BUILD-ID]
// ADDRESS is a module-relative (link-time) hexadecimal value. A line may also
// be just an address when the previous non-empty line named a module.
std::vector<StackRecord> read_stack_file(const std::string& path);

} // namespace nix_sym
