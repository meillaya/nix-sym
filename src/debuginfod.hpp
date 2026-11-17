#pragma once

#include <string>
#include <vector>

namespace nix_sym {

struct DebuginfodResult {
  bool ok = false;
  std::string path;
  std::string error;
};

// Thin wrapper around elfutils' libdebuginfod client. The client fetches
// debug info and source by build id from the URLs in DEBUGINFOD_URLS, or from
// the URLs passed explicitly.
class DebuginfodClient {
public:
  // An empty URL list means "use DEBUGINFOD_URLS". offline disables the
  // client completely.
  DebuginfodClient(const std::string& cache_root, std::vector<std::string> urls, bool offline);
  ~DebuginfodClient();

  DebuginfodClient(const DebuginfodClient&) = delete;
  DebuginfodClient& operator=(const DebuginfodClient&) = delete;

  bool enabled() const;
  const std::vector<std::string>& urls() const;

  // build_id_hex is the lowercase hexadecimal build id. On success, path is
  // the local file returned by debuginfod.
  DebuginfodResult find_debuginfo(const std::string& build_id_hex);
  DebuginfodResult find_source(const std::string& build_id_hex, const std::string& source_path);

  // Install a fetched debug file in a libdwfl-compatible build-id layout and
  // return the synthetic debug output directory. The directory can be added
  // to debuginfo_path.
  std::string install_debuginfo(const std::string& build_id_hex, const std::string& fetched_path,
                                std::string* error);

  // Copy a fetched source file into the common source mirror so gdb can use a
  // substitute-path mapping. Returns the mirrored path.
  std::string install_source(const std::string& build_id_hex, const std::string& source_path,
                             const std::string& fetched_path);

  // Root of the source mirror. A source path /build/foo/a.c becomes
  // <source_root>/build/foo/a.c.
  std::string source_root() const;

  const std::string& cache_root() const {
    return cache_root_;
  }

private:
  struct Impl;
  Impl* impl_;
  std::string cache_root_;
  bool offline_;
  std::vector<std::string> urls_;
};

} // namespace nix_sym
