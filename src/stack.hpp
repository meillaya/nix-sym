#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nix_sym {

// One address to symbolize from a --stack description.
//
// Addresses are normally module-relative (link-time) addresses, matching
// dwfl_report_elf(base 0, add_p_vaddr=true). The runtime forms below carry a
// runtime address plus the module load base instead; the link-time address used
// for symbol and DWARF lookup is then address - load_base.
struct StackRecord {
  std::string module;
  std::string build_id;
  std::uint64_t address = 0;
  std::size_t line = 0;
  // True for "MODULE RUNTIME_ADDRESS @ LOAD_BASE" and
  // "BUILD-ID RUNTIME_ADDRESS @ LOAD_BASE" records. address is then a runtime
  // address and load_base holds the load base to subtract for lookup.
  bool runtime_address = false;
  std::uint64_t load_base = 0;
};

// Parse the text form of a --stack description. One record per line:
//
//   MODULE ADDRESS [BUILD-ID]          module-relative address
//   MODULE RUNTIME_ADDRESS @ LOAD_BASE runtime address and load base
//   BUILD-ID ADDRESS [@ LOAD_BASE]     no module path; resolved by build id
//   ADDRESS                            reuses the previous module/build id
//
// A token is treated as a build id when it is hexadecimal with an even length
// of at least 8 characters; module paths therefore keep their slash or other
// non-hex characters and are not mistaken for build ids.
//
// '#' starts a comment and blank lines are ignored. ADDRESS, RUNTIME_ADDRESS,
// and LOAD_BASE use C integer syntax with base 0, so "0x1187" is hexadecimal.
// All tokens are validated; malformed input throws std::runtime_error naming
// the one-based line number.
std::vector<StackRecord> parse_stack_text(std::string_view text);

// Read a stack file and return its records. "-" reads standard input. Throws
// std::runtime_error when the file cannot be opened or parsed.
std::vector<StackRecord> read_stack_file(const std::string& path);

// Compute the address used for symbol and DWARF lookup: address - load_base for
// runtime-address records, address otherwise. Returns false and leaves
// *lookup_address untouched when a runtime address is below its load base.
// parse_stack_text rejects such records, so this is a guard for records built
// by hand or by future formats.
bool stack_record_lookup_address(const StackRecord& record, std::uint64_t* lookup_address);

} // namespace nix_sym
