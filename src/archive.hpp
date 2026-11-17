#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nix_sym {
namespace archive {

// Resource limits applied when extracting archives from untrusted paths. The
// defaults are generous enough for real Nix source trees while still bounding
// decompression bombs and absurd archive metadata. Fuzzers pass much smaller
// values.
struct ArchiveLimits {
  // Total number of bytes written across all regular file entries.
  std::uint64_t max_total_bytes = 64ULL * 1024 * 1024 * 1024;
  // Largest single regular file or sparse file offset.
  std::uint64_t max_entry_bytes = 8ULL * 1024 * 1024 * 1024;
  // Maximum number of archive entries processed.
  std::uint64_t max_entries = 4000000;
  // Longest accepted relative path or symlink target.
  std::size_t max_path_length = 4096;
};

// True when path is a non-empty relative path with no component equal to "..".
// A leading '/' is rejected. Matches the historical SourceResolver rules so
// existing accepted archives keep working.
bool safe_relative_path(const std::string& path);

// Copies a directory tree (followed symlinks are never descended) into a new
// destination directory. Regular files, directories and symlinks are copied;
// other entry types are skipped. Returns false and sets *status on failure.
bool copy_directory_tree(const std::string& source, const std::string& destination,
                         std::string* status);

// Applies overlay (optionally rooted at root_hint) on top of destination.
// Writes never follow symlinks inside destination: an overlay entry whose
// parent directory in destination is a symlink is rejected with
// "source-unsafe" instead of escaping the tree. Returns false and sets *status
// on failure.
bool apply_overlay(const std::string& destination, const std::string& overlay,
                   const std::string& root_hint, std::string* status);

// Extracts archive_path into destination, which the caller must have created as
// an empty directory. Supports the formats/filters enabled by libarchive.
//
// Rejects: absolute paths, ".." path components, writes through symlinks,
// oversized entries and absurd entry counts. Symlinks with absolute or ".."
// targets, device nodes, FIFOs and sockets are skipped (never created or
// followed). Partial output is left behind on failure; the caller removes it.
//
// On success sets *status to "resolved". On failure sets one of
// "source-unavailable" (invalid/truncated archive), "cache-error" (I/O error)
// or "source-unsafe" (path traversal, symlink escape, limit exceeded), and
// returns false.
bool extract_archive(const std::string& archive_path, const std::string& destination,
                     const ArchiveLimits& limits, std::string* status);

} // namespace archive
} // namespace nix_sym
