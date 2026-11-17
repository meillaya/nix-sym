#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>

#include "core_reader.hpp"

// fuzz/core_fuzz.cpp -- libFuzzer entry point for the core-file parser.
//
// The input is copied into an anonymous in-memory file and parsed with
// read_core_info_fd(), which validates the ELF type and note payloads without
// spawning processes or touching named filesystems. Hard resource limits keep
// note counts and zstd decompression bounded. Inputs above 256 KiB are skipped:
// real cores are much larger, but small inputs reach every parser path faster.

namespace {

constexpr std::size_t kMaxInput = 256 * 1024;

nix_sym::CoreLimits fuzz_limits() {
  nix_sym::CoreLimits limits;
  limits.max_file_bytes = kMaxInput;
  limits.max_decompressed_bytes = kMaxInput;
  limits.max_note_bytes = 64 * 1024;
  limits.max_notes = 1024;
  return limits;
}

bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    ssize_t n = write(fd, data + written, size - written);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > kMaxInput) {
    return 0;
  }
  int fd = memfd_create("nix-sym-fuzz-core", MFD_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  if (!write_all(fd, data, size) || lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    return 0;
  }
  try {
    (void)nix_sym::read_core_info_fd(fd, fuzz_limits());
  } catch (const std::exception&) {
    // Malformed inputs are expected to throw; only crashes matter here.
  } catch (...) {
  }
  close(fd);
  return 0;
}
