#include <cstddef>
#include <cstdint>
#include <string_view>

#include "demangle.hpp"

// fuzz/demangle_fuzz.cpp -- libFuzzer entry point for the symbol demanglers.
//
// The bytes are not guaranteed to be NUL terminated, so always carry the size
// along with the data pointer. Inputs above 4 KiB are skipped: real symbols are
// far smaller, and the demanglers are not designed for huge strings.

namespace {

constexpr std::size_t kMaxInput = 4096;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > kMaxInput) {
    return 0;
  }

  const std::string_view input(reinterpret_cast<const char*>(data), size);
  (void)nix_sym::demangle_rust(input);
  (void)nix_sym::demangle_haskell(input);
  (void)nix_sym::demangle_cxx(input);
  return 0;
}
