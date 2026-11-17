#include "archive.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace nix_sym {
namespace archive {
namespace {

namespace fs = std::filesystem;

std::vector<std::string> split_components(const std::string& path) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : path) {
    if (c == '/') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

std::string strip_leading_dot_slashes(std::string path) {
  while (path.rfind("./", 0) == 0) {
    path.erase(0, 2);
  }
  return path;
}

bool is_directory(const std::string& path) {
  std::error_code ec;
  return fs::is_directory(path, ec);
}

bool is_regular(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

bool copy_file(const std::string& source, const std::string& destination) {
  std::error_code ec;
  fs::create_directories(fs::path(destination).parent_path(), ec);
  fs::remove(destination, ec);
  fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

bool hardlink_or_copy(const std::string& source, const std::string& destination) {
  std::error_code ec;
  fs::create_directories(fs::path(destination).parent_path(), ec);
  fs::remove(destination, ec);
  fs::create_hard_link(source, destination, ec);
  if (!ec) {
    return true;
  }
  return copy_file(source, destination);
}

bool copy_symlink(const std::string& source, const std::string& destination) {
  std::error_code ec;
  fs::path target = fs::read_symlink(source, ec);
  if (ec) {
    return false;
  }
  fs::remove(destination, ec);
  fs::create_directories(fs::path(destination).parent_path(), ec);
  fs::create_symlink(target, destination, ec);
  return !ec;
}

bool unlink_non_directory(const std::string& target, std::string* status) {
  struct stat st;
  if (lstat(target.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      *status = "source-unsafe";
      return false;
    }
    if (unlink(target.c_str()) != 0) {
      *status = "cache-error";
      return false;
    }
    return true;
  }
  if (errno != ENOENT) {
    *status = "cache-error";
    return false;
  }
  return true;
}

// True when the existing filesystem object at path is a real directory. The
// destination tree must never contain a symlink in a component we write
// through, so symlinks and other non-directories are rejected.
bool ensure_parent_directories(const std::string& destination,
                               const std::vector<std::string>& components, std::string* status) {
  std::string current = destination;
  for (std::size_t i = 0; i + 1 < components.size(); ++i) {
    current += "/";
    current += components[i];
    struct stat st;
    if (lstat(current.c_str(), &st) == 0) {
      if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        *status = "source-unsafe";
        return false;
      }
    } else if (errno == ENOENT) {
      if (mkdir(current.c_str(), 0755) != 0) {
        *status = "cache-error";
        return false;
      }
    } else {
      *status = "cache-error";
      return false;
    }
  }
  return true;
}

bool append_directory(const std::string& destination, const std::string& relative,
                      const std::vector<std::string>& components, std::string* status) {
  if (!ensure_parent_directories(destination, components, status)) {
    return false;
  }
  const std::string target = destination + "/" + relative;
  struct stat st;
  if (lstat(target.c_str(), &st) == 0) {
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
      *status = "source-unsafe";
      return false;
    }
    return true;
  }
  if (errno != ENOENT) {
    *status = "cache-error";
    return false;
  }
  if (mkdir(target.c_str(), 0755) != 0) {
    *status = "cache-error";
    return false;
  }
  return true;
}

// Removes a pre-existing symlink/file at a final component and opens a fresh
// file with O_EXCL|O_NOFOLLOW so an archive cannot trick us into writing
// through a symlink it created earlier.
int create_fresh_file(const std::string& destination, const std::string& relative,
                      const std::vector<std::string>& components, std::string* status) {
  if (!ensure_parent_directories(destination, components, status)) {
    return -1;
  }
  const std::string target = destination + "/" + relative;
  if (!unlink_non_directory(target, status)) {
    return -1;
  }
  int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) {
    *status = "cache-error";
    return -1;
  }
  return fd;
}

struct FdHandle {
  int value = -1;
  explicit FdHandle(int fd) : value(fd) {}
  ~FdHandle() {
    if (value >= 0) {
      close(value);
    }
  }
  FdHandle(const FdHandle&) = delete;
  FdHandle& operator=(const FdHandle&) = delete;
};

struct ArchiveHandle {
  struct archive* value = nullptr;
  ArchiveHandle() : value(archive_read_new()) {}
  ~ArchiveHandle() {
    if (value != nullptr) {
      archive_read_free(value);
    }
  }
  ArchiveHandle(const ArchiveHandle&) = delete;
  ArchiveHandle& operator=(const ArchiveHandle&) = delete;
};

bool write_block(int fd, const void* buffer, std::size_t size, std::uint64_t offset,
                 std::string* status) {
  const char* data = static_cast<const char*>(buffer);
  std::size_t written = 0;
  while (written < size) {
    ssize_t n = pwrite(fd, data + written, size - written, static_cast<off_t>(offset + written));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      *status = "cache-error";
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  return true;
}

bool extract_regular_file(const std::string& destination, const std::string& relative,
                          const std::vector<std::string>& components, struct archive* reader,
                          const ArchiveLimits& limits, std::uint64_t* total_written,
                          std::string* status) {
  int raw_fd = create_fresh_file(destination, relative, components, status);
  if (raw_fd < 0) {
    return false;
  }
  FdHandle fd(raw_fd);
  while (true) {
    const void* buffer = nullptr;
    std::size_t size = 0;
    la_int64_t offset = 0;
    const int result = archive_read_data_block(reader, &buffer, &size, &offset);
    if (result == ARCHIVE_EOF) {
      break;
    }
    if (result != ARCHIVE_OK) {
      *status = "source-unavailable";
      return false;
    }
    if (size == 0) {
      continue;
    }
    if (buffer == nullptr) {
      *status = "source-unavailable";
      return false;
    }
    if (offset < 0) {
      *status = "source-unsafe";
      return false;
    }
    const std::uint64_t block_offset = static_cast<std::uint64_t>(offset);
    if (block_offset > limits.max_entry_bytes || size > limits.max_entry_bytes - block_offset) {
      *status = "source-unsafe";
      return false;
    }
    if (size > limits.max_total_bytes - *total_written) {
      *status = "source-unsafe";
      return false;
    }
    if (!write_block(fd.value, buffer, size, block_offset, status)) {
      return false;
    }
    *total_written += size;
  }
  return true;
}

bool extract_hardlink(const std::string& destination, const std::string& relative,
                      const std::vector<std::string>& components, const char* target_name,
                      std::string* status) {
  if (target_name == nullptr) {
    return true;
  }
  const std::string link_relative = strip_leading_dot_slashes(target_name);
  if (!safe_relative_path(link_relative)) {
    // A hard link pointing outside the extraction root is never created.
    return true;
  }
  const std::string link_source = destination + "/" + link_relative;
  struct stat st;
  if (lstat(link_source.c_str(), &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
    return true;
  }
  if (!ensure_parent_directories(destination, components, status)) {
    return false;
  }
  const std::string target = destination + "/" + relative;
  if (!unlink_non_directory(target, status)) {
    return false;
  }
  if (link(link_source.c_str(), target.c_str()) != 0) {
    *status = "cache-error";
    return false;
  }
  return true;
}

bool extract_symlink(const std::string& destination, const std::string& relative,
                     const std::vector<std::string>& components, const char* link_name,
                     const ArchiveLimits& limits, std::string* status) {
  if (link_name == nullptr) {
    return true;
  }
  const std::string link(link_name);
  // Never create a symlink that can resolve outside the extraction root. The
  // historical SourceResolver behavior skipped such entries, so keep skipping
  // them rather than failing the whole archive.
  if (link.empty() || link.size() > limits.max_path_length || link[0] == '/' ||
      link.find("..") != std::string::npos) {
    return true;
  }
  if (!ensure_parent_directories(destination, components, status)) {
    return false;
  }
  const std::string target = destination + "/" + relative;
  if (!unlink_non_directory(target, status)) {
    return false;
  }
  if (symlink(link.c_str(), target.c_str()) != 0) {
    *status = "cache-error";
    return false;
  }
  return true;
}

} // namespace

bool safe_relative_path(const std::string& path) {
  if (path.empty() || path[0] == '/' || path.find('\0') != std::string::npos) {
    return false;
  }
  for (const auto& part : split_components(path)) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

bool copy_directory_tree(const std::string& source, const std::string& destination,
                         std::string* status) {
  std::error_code ec;
  fs::create_directories(destination, ec);
  if (ec) {
    *status = "cache-error";
    return false;
  }
  for (fs::recursive_directory_iterator iterator(source, ec), end; iterator != end;
       iterator.increment(ec)) {
    if (ec) {
      *status = "source-unavailable";
      return false;
    }
    const fs::path& entry = iterator->path();
    std::string relative = fs::relative(entry, source, ec).generic_string();
    if (ec || !safe_relative_path(relative)) {
      *status = "source-unsafe";
      return false;
    }
    std::string target = destination + "/" + relative;
    if (iterator->is_symlink(ec)) {
      if (!copy_symlink(entry.string(), target)) {
        *status = "source-unavailable";
        return false;
      }
    } else if (iterator->is_directory(ec)) {
      fs::create_directories(target, ec);
    } else if (iterator->is_regular_file(ec)) {
      if (!hardlink_or_copy(entry.string(), target)) {
        *status = "source-unavailable";
        return false;
      }
    }
  }
  return true;
}

bool apply_overlay(const std::string& destination, const std::string& overlay,
                   const std::string& root_hint, std::string* status) {
  if (overlay.empty() || !is_directory(overlay)) {
    return true;
  }
  if (!is_directory(destination)) {
    *status = "cache-error";
    return false;
  }
  std::string base = overlay;
  std::error_code ec;
  if (!root_hint.empty() && is_directory(overlay + "/" + root_hint)) {
    base = overlay + "/" + root_hint;
  }
  for (fs::recursive_directory_iterator iterator(base, ec), end; iterator != end;
       iterator.increment(ec)) {
    if (ec) {
      *status = "source-unavailable";
      return false;
    }
    const fs::path& entry = iterator->path();
    std::string relative = fs::relative(entry, base, ec).generic_string();
    if (ec || !safe_relative_path(relative)) {
      *status = "source-unsafe";
      return false;
    }
    const std::vector<std::string> components = split_components(relative);
    if (iterator->is_symlink(ec)) {
      fs::path link = fs::read_symlink(entry, ec);
      if (ec) {
        *status = "source-unavailable";
        return false;
      }
      if (!ensure_parent_directories(destination, components, status)) {
        return false;
      }
      const std::string target = destination + "/" + relative;
      if (!unlink_non_directory(target, status)) {
        return false;
      }
      fs::create_symlink(link, target, ec);
      if (ec) {
        *status = "cache-error";
        return false;
      }
    } else if (iterator->is_directory(ec)) {
      if (!append_directory(destination, relative, components, status)) {
        return false;
      }
    } else if (iterator->is_regular_file(ec)) {
      if (!ensure_parent_directories(destination, components, status)) {
        return false;
      }
      const std::string target = destination + "/" + relative;
      // The parent components are verified real directories, so after removing
      // any previous entry at the final component the copy cannot follow a
      // symlink out of the destination tree.
      fs::remove(target, ec);
      ec.clear();
      fs::copy_file(entry.string(), target, fs::copy_options::overwrite_existing, ec);
      if (ec) {
        *status = "cache-error";
        return false;
      }
    }
    // Other entry types (devices, FIFOs, sockets) are never copied.
  }
  return true;
}

bool extract_archive(const std::string& archive_path, const std::string& destination,
                     const ArchiveLimits& limits, std::string* status) {
  if (!is_regular(archive_path) || !is_directory(destination)) {
    *status = "source-unavailable";
    return false;
  }
  ArchiveHandle reader;
  if (reader.value == nullptr) {
    *status = "cache-error";
    return false;
  }
  archive_read_support_filter_all(reader.value);
  archive_read_support_format_all(reader.value);
  if (archive_read_open_filename(reader.value, archive_path.c_str(), 65536) != ARCHIVE_OK) {
    *status = "source-unavailable";
    return false;
  }

  std::uint64_t entries = 0;
  std::uint64_t total_written = 0;
  std::uint64_t total_declared = 0;

  while (true) {
    struct archive_entry* entry = nullptr;
    const int header = archive_read_next_header(reader.value, &entry);
    if (header == ARCHIVE_EOF) {
      break;
    }
    if (header != ARCHIVE_OK && header != ARCHIVE_WARN) {
      *status = "source-unavailable";
      return false;
    }
    if (entry == nullptr) {
      continue;
    }
    if (++entries > limits.max_entries) {
      *status = "source-unsafe";
      return false;
    }
    const char* raw_path = archive_entry_pathname(entry);
    if (raw_path == nullptr) {
      continue;
    }
    const std::string relative = strip_leading_dot_slashes(raw_path);
    if (relative.size() > limits.max_path_length) {
      *status = "source-unsafe";
      return false;
    }
    if (!safe_relative_path(relative)) {
      *status = "source-unsafe";
      return false;
    }
    const std::vector<std::string> components = split_components(relative);
    const mode_t type = archive_entry_filetype(entry);
    const char* hardlink_target = archive_entry_hardlink(entry);
    if (type == AE_IFDIR) {
      if (!append_directory(destination, relative, components, status)) {
        return false;
      }
      continue;
    }
    if (hardlink_target != nullptr && type == 0) {
      // libarchive reports tar hard link entries with a zero file type; the
      // link target is carried separately. Only create links whose target is
      // already inside the extraction root.
      if (!extract_hardlink(destination, relative, components, hardlink_target, status)) {
        return false;
      }
      continue;
    }
    if (type == AE_IFLNK) {
      if (!extract_symlink(destination, relative, components, archive_entry_symlink(entry), limits,
                           status)) {
        return false;
      }
      continue;
    }
    if (type != AE_IFREG) {
      // Devices, FIFOs, sockets and unknown entry types are never created.
      continue;
    }
    if (hardlink_target != nullptr) {
      if (!extract_hardlink(destination, relative, components, hardlink_target, status)) {
        return false;
      }
      continue;
    }
    const la_int64_t declared_size = archive_entry_size(entry);
    if (declared_size < 0) {
      *status = "source-unavailable";
      return false;
    }
    const std::uint64_t declared = static_cast<std::uint64_t>(declared_size);
    if (declared > limits.max_entry_bytes) {
      *status = "source-unsafe";
      return false;
    }
    if (declared > limits.max_total_bytes - total_declared) {
      *status = "source-unsafe";
      return false;
    }
    total_declared += declared;
    if (!extract_regular_file(destination, relative, components, reader.value, limits,
                              &total_written, status)) {
      return false;
    }
  }

  *status = "resolved";
  return true;
}

} // namespace archive
} // namespace nix_sym
