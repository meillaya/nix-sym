#include "source.hpp"

#include "debuginfod.hpp"

#include <fcntl.h>

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nix_sym {
namespace {

namespace fs = std::filesystem;

std::string basename(const std::string& path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string fnv_hex(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

bool path_is_within(const std::string& path, const std::string& root) {
  if (root.empty()) {
    return false;
  }
  if (path == root) {
    return true;
  }
  return path.rfind(root + "/", 0) == 0;
}

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

bool safe_relative_path(const std::string& path) {
  if (path.empty() || path[0] == '/') {
    return false;
  }
  for (const auto& part : split_components(path)) {
    if (part == "..") {
      return false;
    }
  }
  return true;
}

bool is_directory(const std::string& path) {
  std::error_code ec;
  return fs::is_directory(path, ec);
}

bool is_regular(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

bool exists(const std::string& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

bool create_directories(const std::string& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  return !ec;
}

void remove_all(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

bool write_text(const std::string& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << content;
  return static_cast<bool>(output);
}

std::string read_text(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
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
    std::string target = destination + "/" + relative;
    if (iterator->is_symlink(ec)) {
      if (!copy_symlink(entry.string(), target)) {
        *status = "source-unavailable";
        return false;
      }
    } else if (iterator->is_directory(ec)) {
      fs::create_directories(target, ec);
    } else if (iterator->is_regular_file(ec)) {
      if (!copy_file(entry.string(), target)) {
        *status = "source-unavailable";
        return false;
      }
    }
  }
  return true;
}

} // namespace

SourceResolver::SourceResolver(const std::string& cache_root, bool materialize, bool verbose,
                               const std::vector<std::string>& source_dirs,
                               DebuginfodClient* debuginfod)
    : cache_root_(cache_root), materialize_(materialize), verbose_(verbose),
      debuginfod_(debuginfod) {
  for (const auto& mapping : source_dirs) {
    auto equals = mapping.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= mapping.size()) {
      continue;
    }
    std::string prefix = mapping.substr(0, equals);
    std::string directory = mapping.substr(equals + 1);
    while (prefix.size() > 1 && prefix.back() == '/') {
      prefix.pop_back();
    }
    while (directory.size() > 1 && directory.back() == '/') {
      directory.pop_back();
    }
    source_maps_.emplace_back(prefix, directory);
  }
}

SourceMapping SourceResolver::module_mapping(const ModuleInfo& module) {
  SourceMapping mapping;
  mapping.module = module.path.empty() ? module.name : module.path;
  mapping.source_path = module.source_path;
  mapping.overlay_path = module.overlay_path;
  if (module.source_path.empty()) {
    mapping.status = "unavailable";
    return mapping;
  }
  if (is_directory(module.source_path)) {
    mapping.kind = "directory";
    mapping.status = "available";
  } else if (is_regular(module.source_path)) {
    mapping.kind = "archive";
    mapping.status = "available";
  } else {
    mapping.kind = "none";
    mapping.status = "unavailable";
  }
  return mapping;
}

std::string SourceResolver::find_source_root(const std::string& base,
                                             const std::string& root_hint) {
  if (base.empty()) {
    return base;
  }
  if (!root_hint.empty() && is_directory(base + "/" + root_hint)) {
    return base + "/" + root_hint;
  }
  std::string base_name = basename(base);
  if (base_name == root_hint || (!root_hint.empty() && base_name.size() > root_hint.size() + 1 &&
                                 base_name.compare(base_name.size() - root_hint.size() - 1,
                                                   root_hint.size() + 1, "-" + root_hint) == 0)) {
    return base;
  }
  return base;
}

std::string find_extracted_root(const std::string& base, const std::string& root_hint) {
  if (!root_hint.empty() && is_directory(base + "/" + root_hint)) {
    return base + "/" + root_hint;
  }
  std::error_code ec;
  std::string single;
  int directories = 0;
  int regular_files = 0;
  for (fs::directory_iterator iterator(base, ec), end; iterator != end; iterator.increment(ec)) {
    if (ec) {
      break;
    }
    if (iterator->is_directory(ec)) {
      ++directories;
      single = iterator->path().string();
    } else if (iterator->is_regular_file(ec)) {
      ++regular_files;
    }
  }
  if (directories == 1 && regular_files == 0) {
    return single;
  }
  return base;
}

bool SourceResolver::extract_archive(const std::string& archive_path,
                                     const std::string& destination, const std::string& root_hint,
                                     std::string* extracted_root, std::string* status) {
  if (!is_regular(archive_path)) {
    *status = "source-unavailable";
    return false;
  }
  remove_all(destination);
  if (!create_directories(destination)) {
    *status = "cache-error";
    return false;
  }

  struct archive* archive = archive_read_new();
  if (archive == nullptr) {
    *status = "cache-error";
    return false;
  }
  archive_read_support_filter_all(archive);
  archive_read_support_format_all(archive);
  if (archive_read_open_filename(archive, archive_path.c_str(), 65536) != ARCHIVE_OK) {
    *status = "source-unavailable";
    archive_read_free(archive);
    return false;
  }

  bool ok = true;
  struct archive_entry* entry = nullptr;
  while (archive_read_next_header(archive, &entry) == ARCHIVE_OK) {
    const char* raw_path = archive_entry_pathname(entry);
    if (raw_path == nullptr) {
      continue;
    }
    std::string relative = raw_path;
    while (relative.rfind("./", 0) == 0) {
      relative.erase(0, 2);
    }
    if (!safe_relative_path(relative)) {
      *status = "source-unsafe";
      ok = false;
      break;
    }
    std::string target = destination + "/" + relative;
    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) {
      fs::create_directories(target);
    } else if (type == AE_IFREG) {
      fs::create_directories(fs::path(target).parent_path());
      int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) {
        *status = "cache-error";
        ok = false;
        break;
      }
      const void* buffer = nullptr;
      size_t size = 0;
      la_int64_t offset = 0;
      bool file_ok = true;
      while (archive_read_data_block(archive, &buffer, &size, &offset) == ARCHIVE_OK) {
        const char* data = static_cast<const char*>(buffer);
        size_t written = 0;
        while (written < size) {
          ssize_t n = pwrite(fd, data + written, size - written,
                             static_cast<off_t>(offset + static_cast<la_int64_t>(written)));
          if (n < 0 && errno == EINTR) {
            continue;
          }
          if (n <= 0) {
            file_ok = false;
            break;
          }
          written += static_cast<size_t>(n);
        }
        if (!file_ok) {
          break;
        }
      }
      close(fd);
      if (!file_ok) {
        *status = "cache-error";
        ok = false;
        break;
      }
    } else if (type == AE_IFLNK) {
      const char* link_target = archive_entry_symlink(entry);
      if (link_target != nullptr) {
        std::string link(link_target);
        if (link.find("..") == std::string::npos && link[0] != '/') {
          fs::create_directories(fs::path(target).parent_path());
          fs::remove(target);
          fs::create_symlink(link, target);
        }
      }
    }
  }
  archive_read_free(archive);
  if (!ok) {
    remove_all(destination);
    return false;
  }
  *extracted_root = find_extracted_root(destination, root_hint);
  *status = "resolved";
  return true;
}

bool SourceResolver::copy_tree_with_overlay(const std::string& source, const std::string& overlay,
                                            const std::string& root_hint,
                                            const std::string& destination, std::string* status) {
  remove_all(destination);
  std::string source_root = find_source_root(source, root_hint);
  if (!copy_directory_tree(source_root, destination, status)) {
    remove_all(destination);
    return false;
  }
  if (!apply_overlay(destination, overlay, root_hint, status)) {
    remove_all(destination);
    return false;
  }
  *status = "resolved";
  return true;
}

bool SourceResolver::ensure_source(const SourceMapping& mapping, std::string* resolved_root,
                                   std::string* status) {
  if (mapping.source_path.empty()) {
    *status = "unavailable";
    return false;
  }
  if (!exists(mapping.source_path)) {
    *status = "source-unavailable";
    return false;
  }
  std::string key =
      hash_text(mapping.source_path + "|" + mapping.overlay_path + "|" + mapping.root);
  std::string cache_dir = cache_root_ + "/sources/" + key;
  std::string marker = cache_dir + "/.complete";
  std::string metadata = cache_dir + "/root.txt";

  if (exists(marker)) {
    std::string stored_root = read_text(metadata);
    while (!stored_root.empty() && (stored_root.back() == '\n' || stored_root.back() == '\r')) {
      stored_root.pop_back();
    }
    if (!stored_root.empty() && is_directory(stored_root)) {
      *resolved_root = stored_root;
      *status = "resolved";
      return true;
    }
  }

  bool materialize = materialize_;
  if (!materialize) {
    *status = "available";
    return false;
  }

  if (mapping.kind == "directory" && mapping.overlay_path.empty()) {
    std::string root = find_source_root(mapping.source_path, mapping.root);
    *resolved_root = root;
    *status = "resolved";
    return true;
  }

  create_directories(cache_dir);
  std::string extracted;
  bool ok = false;
  if (mapping.kind == "archive") {
    ok =
        extract_archive(mapping.source_path, cache_dir + "/tree", mapping.root, &extracted, status);
  } else if (mapping.kind == "directory") {
    ok = copy_tree_with_overlay(mapping.source_path, mapping.overlay_path, mapping.root,
                                cache_dir + "/tree", status);
    if (ok) {
      extracted = cache_dir + "/tree";
    }
  } else {
    *status = "unavailable";
    return false;
  }
  if (!ok) {
    return false;
  }
  if (mapping.kind == "archive" && !mapping.overlay_path.empty()) {
    std::string overlay_status;
    if (!apply_overlay(extracted, mapping.overlay_path, mapping.root, &overlay_status)) {
      *status = overlay_status;
      return false;
    }
  }
  write_text(metadata, extracted);
  write_text(marker, "ok\n");
  *resolved_root = extracted;
  *status = "resolved";
  return true;
}

SourceLocation SourceResolver::map(const ModuleInfo& module, const std::string& comp_dir,
                                   const std::string& file, bool force_materialize) {
  SourceLocation location;
  if (file.empty()) {
    return location;
  }
  location.original = file;
  location.comp_dir = comp_dir;
  location.valid = true;
  location.status = "unresolved";

  std::string root_path;
  std::string relative;
  std::string normalized_comp_dir = comp_dir;
  while (!normalized_comp_dir.empty() && normalized_comp_dir.back() == '/') {
    normalized_comp_dir.pop_back();
  }

  std::string full_file = file;
  if (!full_file.empty() && full_file[0] != '/' && !normalized_comp_dir.empty()) {
    full_file = normalized_comp_dir + "/" + full_file;
  }
  location.original = full_file;

  for (const auto& [prefix, directory] : source_maps_) {
    if (full_file.rfind(prefix, 0) != 0) {
      continue;
    }
    std::string suffix = full_file.substr(prefix.size());
    if (!suffix.empty() && suffix.front() != '/') {
      suffix.insert(suffix.begin(), '/');
    }
    std::string candidate = directory + suffix;
    SourceMapping mapping;
    mapping.module = "source-map";
    mapping.comp_dir = prefix;
    mapping.root = basename(prefix);
    mapping.kind = "directory";
    mapping.resolved_root = directory;
    mapping.status = is_regular(candidate) ? "resolved" : "source-missing";
    mappings_.insert_or_assign("source-map|" + prefix, mapping);
    if (is_regular(candidate)) {
      location.resolved = candidate;
      location.status = "resolved";
    } else {
      location.status = "source-missing";
    }
    return location;
  }

  if (!normalized_comp_dir.empty() && path_is_within(full_file, normalized_comp_dir)) {
    root_path = normalized_comp_dir;
    relative = full_file.substr(normalized_comp_dir.size());
    while (!relative.empty() && relative.front() == '/') {
      relative.erase(relative.begin());
    }
  } else if (full_file.rfind("/build/", 0) == 0) {
    size_t end = full_file.find('/', 7);
    if (end != std::string::npos) {
      root_path = full_file.substr(0, end);
      relative = full_file.substr(end + 1);
    }
  } else {
    if (!full_file.empty() && full_file[0] == '/') {
      if (is_regular(full_file)) {
        location.resolved = full_file;
        location.status = "resolved";
      } else {
        location.status = "source-unavailable";
      }
    }
    return location;
  }

  std::string root_name = basename(root_path);
  if (root_name.empty()) {
    return location;
  }

  std::string mapping_key = module.path.empty() ? module.name : module.path;
  mapping_key += "|" + root_path;
  auto found = mappings_.find(mapping_key);
  if (found == mappings_.end()) {
    SourceMapping mapping = module_mapping(module);
    mapping.root = root_name;
    mapping.comp_dir = root_path;
    if (mapping.status == "available") {
      std::string resolved_root;
      std::string status;
      bool materialize = materialize_ || force_materialize;
      bool was_materialize = materialize_;
      materialize_ = materialize;
      ensure_source(mapping, &resolved_root, &status);
      materialize_ = was_materialize;
      mapping.resolved_root = resolved_root;
      mapping.status = status;
    }
    found = mappings_.insert_or_assign(mapping_key, mapping).first;
  }

  const SourceMapping& mapping = found->second;
  if (mapping.status == "resolved" && !mapping.resolved_root.empty()) {
    std::string candidate = mapping.resolved_root + "/" + relative;
    if (is_regular(candidate)) {
      location.resolved = candidate;
      location.status = "resolved";
    } else {
      location.status = "source-missing";
    }
  } else {
    location.status = mapping.status;
  }
  if (location.status == "resolved" || debuginfod_ == nullptr || !debuginfod_->enabled() ||
      module.build_id.empty()) {
    return location;
  }

  std::string remote_source = full_file;
  if (remote_source.find("/../") != std::string::npos) {
    remote_source = std::filesystem::path(remote_source).lexically_normal().string();
  }
  DebuginfodResult fetched = debuginfod_->find_source(module.build_id, remote_source);
  if (!fetched.ok) {
    location.status = "debuginfod-failed";
    return location;
  }
  bool materialize = materialize_ || force_materialize;
  if (materialize) {
    std::string mirrored =
        debuginfod_->install_source(module.build_id, remote_source, fetched.path);
    if (mirrored.empty()) {
      location.status = "debuginfod-failed";
      return location;
    }
    location.resolved = mirrored;
    SourceMapping remote;
    remote.module = module.path.empty() ? module.name : module.path;
    remote.comp_dir = root_path;
    remote.root = root_name;
    remote.kind = "debuginfod";
    remote.resolved_root = debuginfod_->source_root() + root_path;
    remote.status = "resolved";
    mappings_.insert_or_assign("debuginfod|" + remote.module + "|" + root_path, remote);
  } else {
    location.resolved = fetched.path;
  }
  location.status = "debuginfod";
  return location;
}

std::vector<SourceMapping> SourceResolver::mappings() const {
  std::vector<SourceMapping> result;
  for (const auto& entry : mappings_) {
    result.push_back(entry.second);
  }
  std::sort(result.begin(), result.end(), [](const SourceMapping& a, const SourceMapping& b) {
    if (a.comp_dir != b.comp_dir) {
      return a.comp_dir < b.comp_dir;
    }
    return a.module < b.module;
  });
  return result;
}

std::string SourceResolver::hash_text(const std::string& text) const {
  return fnv_hex(text);
}

} // namespace nix_sym
