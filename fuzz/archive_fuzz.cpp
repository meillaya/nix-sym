#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "archive.hpp"

// fuzz/archive_fuzz.cpp -- libFuzzer entry point for archive/overlay
// extraction.
//
// Each input is written to a scratch file and extracted with
// archive::extract_archive under tight size/entry limits. A synthetic overlay
// tree (including absolute and ".." escaping symlinks) is then applied on top
// of the extraction result and also onto an empty tree, so symlink-escape and
// write-through protections are exercised as well. Inputs above 256 KiB are
// skipped.

namespace {

constexpr std::size_t kMaxInput = 256 * 1024;

class Workspace {
public:
  Workspace() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
      return;
    }
    std::string pattern = (base / "nix-sym-archive-fuzz-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = mkdtemp(buffer.data());
    if (created == nullptr) {
      return;
    }
    root_ = created;
    overlay_ = root_ + "/overlay";
    valid_ = std::filesystem::create_directories(overlay_ + "/dir", ec) && !ec;
    valid_ = valid_ && write_file(overlay_ + "/normal.txt", "normal\n");
    valid_ = valid_ && ::symlink("/", (overlay_ + "/absolute").c_str()) == 0;
    valid_ = valid_ && ::symlink("..", (overlay_ + "/parent").c_str()) == 0;
    valid_ = valid_ && write_file(overlay_ + "/dir/evil.txt", "evil\n");
  }

  ~Workspace() {
    if (!root_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(root_, ec);
    }
  }

  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  bool valid() const {
    return valid_;
  }

  std::string input_path() const {
    return root_ + "/input";
  }

  std::string destination() const {
    return root_ + "/out";
  }

  const std::string& overlay() const {
    return overlay_;
  }

  static bool write_file(const std::string& path, const std::vector<unsigned char>& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(reinterpret_cast<const char*>(content.data()),
              static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
  }

private:
  static bool write_file(const std::string& path, const std::string& content) {
    return write_file(path, std::vector<unsigned char>(content.begin(), content.end()));
  }

  std::string root_;
  std::string overlay_;
  bool valid_ = false;
};

Workspace& workspace() {
  static Workspace instance;
  return instance;
}

nix_sym::archive::ArchiveLimits fuzz_limits() {
  nix_sym::archive::ArchiveLimits limits;
  limits.max_total_bytes = 1024 * 1024;
  limits.max_entry_bytes = 256 * 1024;
  limits.max_entries = 1024;
  limits.max_path_length = 1024;
  return limits;
}

void reset_destination(Workspace* workspace) {
  std::error_code ec;
  std::filesystem::remove_all(workspace->destination(), ec);
  std::filesystem::create_directories(workspace->destination(), ec);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > kMaxInput) {
    return 0;
  }
  Workspace& ws = workspace();
  if (!ws.valid()) {
    return 0;
  }
  std::vector<unsigned char> input(data, data + size);
  if (!Workspace::write_file(ws.input_path(), input)) {
    return 0;
  }

  const nix_sym::archive::ArchiveLimits limits = fuzz_limits();
  std::string status;

  std::error_code ec;
  std::filesystem::remove_all(ws.destination(), ec);
  std::filesystem::create_directories(ws.destination(), ec);
  if (ec) {
    return 0;
  }
  try {
    (void)nix_sym::archive::extract_archive(ws.input_path(), ws.destination(), limits, &status);
  } catch (const std::exception&) {
  } catch (...) {
  }

  // Apply the adversarial overlay on top of whatever the archive produced.
  try {
    (void)nix_sym::archive::apply_overlay(ws.destination(), ws.overlay(), "", &status);
  } catch (const std::exception&) {
  } catch (...) {
  }

  // Apply the overlay onto an empty tree as well so the overlay path is always
  // exercised, even when extraction rejected the archive.
  reset_destination(&ws);
  try {
    (void)nix_sym::archive::apply_overlay(ws.destination(), ws.overlay(), "", &status);
  } catch (const std::exception&) {
  } catch (...) {
  }
  return 0;
}
