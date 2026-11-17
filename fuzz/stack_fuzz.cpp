#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "stack.hpp"

// fuzz/stack_fuzz.cpp -- libFuzzer entry point for the --stack text parser.
//
// The bytes are not guaranteed to be NUL terminated, so always carry the size
// along with the data pointer. The parser must reject malformed input with
// std::runtime_error and must never read out of bounds or crash.

namespace {

constexpr std::size_t kMaxInput = 64 * 1024;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > kMaxInput) {
    return 0;
  }

  const std::string_view input(reinterpret_cast<const char*>(data), size);
  try {
    const std::vector<nix_sym::StackRecord> records = nix_sym::parse_stack_text(input);
    for (const nix_sym::StackRecord& record : records) {
      std::uint64_t lookup_address = 0;
      (void)nix_sym::stack_record_lookup_address(record, &lookup_address);
    }
  } catch (const std::exception&) {
    // Malformed stack text is expected; read_stack_file reports the line.
  }
  return 0;
}
