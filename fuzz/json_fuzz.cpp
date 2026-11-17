#include <cstddef>
#include <cstdint>
#include <string>

#include "json.hpp"

// fuzz/json_fuzz.cpp -- libFuzzer entry point for the JSON string escaping
// code. The escaping path only needs a byte string, so feed it the raw input.

namespace {

constexpr std::size_t kMaxInput = 8192;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > kMaxInput) {
    return 0;
  }

  const std::string input(reinterpret_cast<const char*>(data), size);
  (void)nix_sym::json_escape(input);
  return 0;
}
