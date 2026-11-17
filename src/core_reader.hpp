#pragma once

#include <elfutils/libdwfl.h>
#include <gelf.h>

#include <cstddef>
#include <string>

#include "model.hpp"

namespace nix_sym {

// Resource limits applied while reading possibly untrusted core images. The
// defaults are deliberately generous so real crash dumps are unaffected;
// fuzzers and tests pass much smaller values.
struct CoreLimits {
  // Largest raw core image (or decompressed image) accepted, in bytes.
  std::size_t max_file_bytes = 64ULL * 1024 * 1024 * 1024;
  // Largest zstd-decompressed core accepted, in bytes. Guards against
  // decompression bombs when reading a compressed core.
  std::size_t max_decompressed_bytes = 64ULL * 1024 * 1024 * 1024;
  // Largest PT_NOTE segment accepted, in bytes, and largest single note
  // descriptor accepted. Anything beyond is treated as malformed.
  std::size_t max_note_bytes = 512ULL * 1024 * 1024;
  // Maximum number of ELF notes parsed from one core image.
  std::size_t max_notes = 1U << 20;
};

struct CoreFile {
  std::string path;
  std::string temp_path;
  bool compressed = false;
  int fd = -1;
  Elf* elf = nullptr;

  CoreFile() = default;
  CoreFile(const CoreFile&) = delete;
  CoreFile& operator=(const CoreFile&) = delete;
  CoreFile(CoreFile&& other) noexcept;
  CoreFile& operator=(CoreFile&& other) noexcept;
  ~CoreFile();

  bool valid() const {
    return elf != nullptr;
  }
};

CoreFile open_core_file(const std::string& path, const std::string& cache_root,
                        const std::string& coredumpctl_pid,
                        const CoreLimits& limits = CoreLimits{});
CoreInfo read_core_info(const CoreFile& core, const CoreLimits& limits = CoreLimits{});

// Parse and validate an ELF core image from an already-open seekable file
// descriptor. Takes no ownership of fd and never closes it.
//
// If the descriptor starts with the zstd magic the image is transparently
// decompressed into an anonymous in-memory file first, bounded by
// limits.max_decompressed_bytes. This entry point never spawns processes and
// never creates filesystem paths, which makes it safe to call from fuzzers.
//
// Returns the parsed core info. Throws std::runtime_error with a descriptive
// message for empty, non-ELF, non-core, truncated, or oversized inputs.
CoreInfo read_core_info_fd(int fd, const CoreLimits& limits = CoreLimits{});

} // namespace nix_sym
