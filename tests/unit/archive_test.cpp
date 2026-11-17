#include "../test.hpp"

#include "archive.hpp"
#include "model.hpp"
#include "source.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using nix_sym::ModuleInfo;
using nix_sym::SourceLocation;
using nix_sym::SourceResolver;
using nix_sym::archive::ArchiveLimits;

int checks = 0;
int groups = 0;

#define CHECK_COUNTED(expr)                                                                        \
  do {                                                                                             \
    ++checks;                                                                                      \
    CHECK(expr);                                                                                   \
  } while (false)

namespace fs = std::filesystem;

struct TempDir {
  std::string path;

  TempDir() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
      return;
    }
    std::string pattern = (base / "nix-sym-archive-test-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = mkdtemp(buffer.data());
    if (created == nullptr) {
      return;
    }
    path = created;
  }

  ~TempDir() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

std::string join(const std::string& directory, const std::string& name) {
  return (fs::path(directory) / name).string();
}

bool write_bytes(const std::string& path, const void* data, std::size_t size) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(out);
}

bool write_text(const std::string& path, const std::string& text) {
  return write_bytes(path, text.data(), text.size());
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::string content;
  char buffer[4096];
  while (in) {
    in.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
    const std::streamsize count = in.gcount();
    if (count > 0) {
      content.append(buffer, static_cast<std::size_t>(count));
    }
  }
  return content;
}

bool path_exists(const std::string& path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0;
}

bool is_symlink(const std::string& path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

// ---------------------------------------------------------------------------
// Synthetic tar construction
// ---------------------------------------------------------------------------

struct RawEntry {
  std::string name;
  char type = '0'; // '0' file, '5' dir, '2' symlink, '1' hardlink, '3' chr, '6' fifo
  std::string content;
  std::string link;
  std::uint32_t major = 1;
  std::uint32_t minor = 3;
};

void put_octal(unsigned char* field, std::size_t width, std::uint64_t value) {
  std::memset(field, '0', width);
  std::string digits;
  if (value == 0) {
    digits = "0";
  } else {
    while (value > 0) {
      digits.push_back(static_cast<char>('0' + (value & 7)));
      value >>= 3;
    }
    std::reverse(digits.begin(), digits.end());
  }
  if (digits.size() > width - 1) {
    digits = digits.substr(digits.size() - (width - 1));
  }
  std::memcpy(field + (width - 1 - digits.size()), digits.data(), digits.size());
  field[width - 1] = '\0';
}

// Builds a raw ustar archive. Used for hostile inputs because libarchive's
// writer normalizes some of them.
std::vector<unsigned char> build_tar(const std::vector<RawEntry>& entries) {
  std::vector<unsigned char> archive;
  for (const RawEntry& entry : entries) {
    std::vector<unsigned char> header(512, 0);
    std::memcpy(header.data(), entry.name.data(), std::min<std::size_t>(entry.name.size(), 100));
    put_octal(header.data() + 100, 8, entry.type == '5' ? 0755 : 0644);
    put_octal(header.data() + 108, 8, 0);
    put_octal(header.data() + 116, 8, 0);
    const std::uint64_t size = (entry.type == '0') ? entry.content.size() : 0;
    put_octal(header.data() + 124, 12, size);
    put_octal(header.data() + 136, 12, 0);
    header[156] = static_cast<unsigned char>(entry.type);
    std::memcpy(header.data() + 157, entry.link.data(),
                std::min<std::size_t>(entry.link.size(), 100));
    std::memcpy(header.data() + 257, "ustar", 6);
    std::memcpy(header.data() + 263, "00", 2);
    if (entry.type == '3') {
      put_octal(header.data() + 329, 8, entry.major);
      put_octal(header.data() + 337, 8, entry.minor);
    }
    std::memset(header.data() + 148, ' ', 8);
    unsigned int checksum = 0;
    for (unsigned char byte : header) {
      checksum += byte;
    }
    char checksum_text[8];
    std::snprintf(checksum_text, sizeof(checksum_text), "%06o", checksum);
    std::memcpy(header.data() + 148, checksum_text, 6);
    header[154] = '\0';
    header[155] = ' ';
    archive.insert(archive.end(), header.begin(), header.end());
    if (size > 0) {
      archive.insert(archive.end(), entry.content.begin(), entry.content.end());
      while (archive.size() % 512 != 0) {
        archive.push_back(0);
      }
    }
  }
  archive.resize(archive.size() + 1024, 0);
  return archive;
}

bool write_archive_entries(const std::string& path, const std::vector<RawEntry>& entries,
                           bool gzip) {
  struct archive* writer = archive_write_new();
  if (writer == nullptr) {
    return false;
  }
  archive_write_set_format_gnutar(writer);
  if (gzip) {
    archive_write_add_filter_gzip(writer);
  }
  if (archive_write_open_filename(writer, path.c_str()) != ARCHIVE_OK) {
    archive_write_free(writer);
    return false;
  }
  bool ok = true;
  for (const RawEntry& raw : entries) {
    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, raw.name.c_str());
    mode_t type = AE_IFREG;
    std::string link = raw.link;
    if (raw.type == '5') {
      type = AE_IFDIR;
    } else if (raw.type == '2') {
      type = AE_IFLNK;
      archive_entry_set_symlink(entry, link.c_str());
    } else if (raw.type == '1') {
      type = AE_IFREG;
      archive_entry_set_hardlink(entry, link.c_str());
    }
    archive_entry_set_filetype(entry, type);
    archive_entry_set_perm(entry, (raw.type == '5') ? 0755 : 0644);
    const std::size_t size = (raw.type == '0') ? raw.content.size() : 0;
    archive_entry_set_size(entry, static_cast<la_int64_t>(size));
    if (archive_write_header(writer, entry) != ARCHIVE_OK) {
      ok = false;
      archive_entry_free(entry);
      break;
    }
    if (size > 0 && archive_write_data(writer, raw.content.data(), raw.content.size()) < 0) {
      ok = false;
      archive_entry_free(entry);
      break;
    }
    archive_entry_free(entry);
  }
  archive_write_close(writer);
  archive_write_free(writer);
  return ok;
}

std::string temp_path(const TempDir& dir, const std::string& name) {
  return join(dir.path, name);
}

void make_directory(const std::string& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
}

bool extract_to(const std::string& archive_path, const std::string& destination,
                const ArchiveLimits& limits, std::string* status) {
  make_directory(destination);
  return nix_sym::archive::extract_archive(archive_path, destination, limits, status);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_safe_relative_path() {
  ++groups;
  CHECK_COUNTED(nix_sym::archive::safe_relative_path("a/b/c"));
  CHECK_COUNTED(nix_sym::archive::safe_relative_path("./a/b"));
  CHECK_COUNTED(nix_sym::archive::safe_relative_path("."));
  CHECK_COUNTED(nix_sym::archive::safe_relative_path("a/./b"));
  CHECK_COUNTED(!nix_sym::archive::safe_relative_path(""));
  CHECK_COUNTED(!nix_sym::archive::safe_relative_path("/etc/passwd"));
  CHECK_COUNTED(!nix_sym::archive::safe_relative_path("../escape"));
  CHECK_COUNTED(!nix_sym::archive::safe_relative_path("a/../../escape"));
  CHECK_COUNTED(!nix_sym::archive::safe_relative_path(std::string("a\0b", 3)));
}

void test_valid_archive_extraction(const TempDir& dir) {
  ++groups;
  const std::string archive = temp_path(dir, "valid.tar.gz");
  const std::vector<RawEntry> entries = {
      {"demo", '5', {}, {}},         {"demo/a.c", '0', "int a;\n", {}},
      {"demo/sub", '5', {}, {}},     {"demo/sub/b.c", '0', "int b;\n", {}},
      {"demo/link", '2', {}, "a.c"}, {"demo/hard", '1', {}, "demo/a.c"},
  };
  CHECK_COUNTED(write_archive_entries(archive, entries, true));

  const std::string destination = temp_path(dir, "valid-out");
  std::string status = "sentinel";
  CHECK_COUNTED(extract_to(archive, destination, ArchiveLimits{}, &status));
  CHECK_COUNTED(status == "resolved");
  CHECK_COUNTED(read_text(join(destination, "demo/a.c")) == "int a;\n");
  CHECK_COUNTED(read_text(join(destination, "demo/sub/b.c")) == "int b;\n");
  CHECK_COUNTED(is_symlink(join(destination, "demo/link")));
  struct stat a_stat;
  struct stat hard_stat;
  CHECK_COUNTED(lstat(join(destination, "demo/a.c").c_str(), &a_stat) == 0);
  CHECK_COUNTED(lstat(join(destination, "demo/hard").c_str(), &hard_stat) == 0);
  CHECK_COUNTED(a_stat.st_ino == hard_stat.st_ino);
}

void test_path_traversal_rejected(const TempDir& dir) {
  ++groups;
  for (const std::string& hostile : {std::string("../escape"), std::string("/absolute/path"),
                                     std::string("safe/../../escape")}) {
    const std::string archive = temp_path(dir, "traversal.tar");
    const std::vector<unsigned char> bytes =
        build_tar({{hostile, '0', "pwned\n", {}}, {"safe.txt", '0', "ok\n", {}}});
    CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));
    const std::string destination = temp_path(dir, "traversal-out");
    std::error_code ec;
    fs::remove_all(destination, ec);
    std::string status;
    CHECK_COUNTED(!extract_to(archive, destination, ArchiveLimits{}, &status));
    CHECK_COUNTED(status == "source-unsafe");
    CHECK_COUNTED(!path_exists(join(dir.path, "escape")));
  }
}

void test_symlink_entries(const TempDir& dir) {
  ++groups;
  // Escaping symlinks are skipped, in-tree symlinks are created.
  const std::string archive = temp_path(dir, "symlinks.tar");
  const std::vector<unsigned char> bytes = build_tar({
      {"good", '2', {}, "target"},
      {"absolute", '2', {}, "/etc/passwd"},
      {"parent", '2', {}, "../outside"},
      {"nested", '2', {}, "sub/thing"},
      {"target", '0', "target\n", {}},
  });
  CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));
  const std::string destination = temp_path(dir, "symlinks-out");
  std::string status;
  CHECK_COUNTED(extract_to(archive, destination, ArchiveLimits{}, &status));
  CHECK_COUNTED(status == "resolved");
  CHECK_COUNTED(is_symlink(join(destination, "good")));
  CHECK_COUNTED(!path_exists(join(destination, "absolute")));
  CHECK_COUNTED(!path_exists(join(destination, "parent")));
  CHECK_COUNTED(is_symlink(join(destination, "nested")));
}

void test_symlink_write_through_rejected(const TempDir& dir) {
  ++groups;
  // A symlink parent followed by a file inside it must not be followed.
  const std::string archive = temp_path(dir, "write-through.tar");
  const std::vector<unsigned char> bytes = build_tar(
      {{"sub", '5', {}, {}}, {"dir", '2', {}, "sub"}, {"dir/file.c", '0', "escaped\n", {}}});
  CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));
  const std::string destination = temp_path(dir, "write-through-out");
  make_directory(join(destination, "sub"));
  std::string status;
  CHECK_COUNTED(!extract_to(archive, destination, ArchiveLimits{}, &status));
  CHECK_COUNTED(status == "source-unsafe");
  CHECK_COUNTED(!path_exists(join(destination, "sub/file.c")));
}

void test_special_entries_skipped(const TempDir& dir) {
  ++groups;
  const std::string archive = temp_path(dir, "special.tar");
  const std::vector<unsigned char> bytes =
      build_tar({{"device", '3', {}, {}, 1, 3}, {"pipe", '6', {}, {}}, {"file", '0', "x", {}}});
  CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));
  const std::string destination = temp_path(dir, "special-out");
  std::string status;
  CHECK_COUNTED(extract_to(archive, destination, ArchiveLimits{}, &status));
  CHECK_COUNTED(status == "resolved");
  CHECK_COUNTED(!path_exists(join(destination, "device")));
  CHECK_COUNTED(!path_exists(join(destination, "pipe")));
  CHECK_COUNTED(path_exists(join(destination, "file")));
}

void test_limits(const TempDir& dir) {
  ++groups;
  // Oversized entry.
  {
    const std::string archive = temp_path(dir, "big.tar");
    const std::vector<unsigned char> bytes = build_tar({{"big", '0', std::string(4096, 'x'), {}}});
    CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));
    ArchiveLimits limits;
    limits.max_entry_bytes = 1024;
    const std::string destination = temp_path(dir, "big-out");
    std::string status;
    CHECK_COUNTED(!extract_to(archive, destination, limits, &status));
    CHECK_COUNTED(status == "source-unsafe");
  }

  // Too many entries.
  {
    const std::string archive = temp_path(dir, "many.tar");
    const std::vector<unsigned char> bytes =
        build_tar({{"a", '0', "1", {}}, {"b", '0', "2", {}}, {"c", '0', "3", {}}});
    CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));
    ArchiveLimits limits;
    limits.max_entries = 2;
    const std::string destination = temp_path(dir, "many-out");
    std::string status;
    CHECK_COUNTED(!extract_to(archive, destination, limits, &status));
    CHECK_COUNTED(status == "source-unsafe");
  }

  // Too long path.
  {
    const std::string archive = temp_path(dir, "long.tar");
    const std::vector<unsigned char> bytes =
        build_tar({{"some/quite/long/path/name/file.c", '0', "x", {}}});
    CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));
    ArchiveLimits limits;
    limits.max_path_length = 8;
    const std::string destination = temp_path(dir, "long-out");
    std::string status;
    CHECK_COUNTED(!extract_to(archive, destination, limits, &status));
    CHECK_COUNTED(status == "source-unsafe");
  }
}

void test_malformed_archives(const TempDir& dir) {
  ++groups;
  const std::string archive = temp_path(dir, "malformed.tar");
  const std::vector<unsigned char> bytes = build_tar({{"a", '0', "content", {}}});
  // Truncate in the middle of the first header.
  CHECK_COUNTED(write_bytes(archive, bytes.data(), 100));
  const std::string destination = temp_path(dir, "malformed-out");
  std::string status;
  CHECK_COUNTED(!extract_to(archive, destination, ArchiveLimits{}, &status));
  CHECK_COUNTED(status == "source-unavailable");

  // Completely bogus bytes.
  const std::string bogus = temp_path(dir, "bogus.bin");
  const std::string junk = "not an archive at all";
  CHECK_COUNTED(write_text(bogus, junk));
  const std::string bogus_out = temp_path(dir, "bogus-out");
  CHECK_COUNTED(!extract_to(bogus, bogus_out, ArchiveLimits{}, &status));
  CHECK_COUNTED(status == "source-unavailable");

  // A truncated gzip stream must be reported, not silently accepted.
  const std::string gzip_path = temp_path(dir, "truncated.tar.gz");
  CHECK_COUNTED(write_archive_entries(gzip_path, {{"demo/a.c", '0', "content\n", {}}}, true));
  std::vector<unsigned char> gzip_bytes;
  {
    std::ifstream input(gzip_path, std::ios::binary);
    gzip_bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  CHECK_COUNTED(gzip_bytes.size() > 8);
  const std::string truncated_gzip = temp_path(dir, "truncated-copy.tar.gz");
  CHECK_COUNTED(write_bytes(truncated_gzip, gzip_bytes.data(), gzip_bytes.size() / 2));
  const std::string gzip_out = temp_path(dir, "gzip-out");
  CHECK_COUNTED(!extract_to(truncated_gzip, gzip_out, ArchiveLimits{}, &status));
  CHECK_COUNTED(status == "source-unavailable");
}

void test_copy_directory_tree_and_overlay(const TempDir& dir) {
  ++groups;
  const std::string source = temp_path(dir, "tree-src");
  make_directory(join(source, "nested"));
  CHECK_COUNTED(write_text(join(source, "a.c"), "original\n"));
  CHECK_COUNTED(write_text(join(source, "nested/b.c"), "nested\n"));

  const std::string destination = temp_path(dir, "tree-dst");
  std::string status;
  CHECK_COUNTED(nix_sym::archive::copy_directory_tree(source, destination, &status));
  CHECK_COUNTED(read_text(join(destination, "a.c")) == "original\n");

  const std::string overlay = temp_path(dir, "overlay");
  make_directory(join(overlay, "nested"));
  CHECK_COUNTED(write_text(join(overlay, "a.c"), "overlaid\n"));
  CHECK_COUNTED(write_text(join(overlay, "nested/c.c"), "new\n"));
  CHECK_COUNTED(nix_sym::archive::apply_overlay(destination, overlay, "", &status));
  CHECK_COUNTED(status.empty());
  CHECK_COUNTED(read_text(join(destination, "a.c")) == "overlaid\n");
  CHECK_COUNTED(read_text(join(destination, "nested/c.c")) == "new\n");
}

void test_overlay_symlink_escape_rejected(const TempDir& dir) {
  ++groups;
  const std::string destination = temp_path(dir, "overlay-dst");
  make_directory(join(destination, "outside"));
  CHECK_COUNTED(write_text(join(destination, "outside/keep.txt"), "keep\n"));
  std::error_code ec;
  fs::create_symlink("outside", join(destination, "link"), ec);
  CHECK_COUNTED(!ec);

  const std::string overlay = temp_path(dir, "overlay-src");
  make_directory(join(overlay, "link"));
  CHECK_COUNTED(write_text(join(overlay, "link/evil.c"), "evil\n"));

  std::string status;
  CHECK_COUNTED(!nix_sym::archive::apply_overlay(destination, overlay, "", &status));
  CHECK_COUNTED(status == "source-unsafe");
  CHECK_COUNTED(!path_exists(join(destination, "outside/evil.c")));
}

// ---------------------------------------------------------------------------
// SourceResolver regression tests: the production path must go through the new
// hardened archive/overlay implementation.
// ---------------------------------------------------------------------------

ModuleInfo make_module(const std::string& source_path, const std::string& overlay_path) {
  ModuleInfo module;
  module.name = "demo";
  module.path = "/nix/store/demo/bin/demo";
  module.source_path = source_path;
  module.overlay_path = overlay_path;
  return module;
}

void test_source_resolver_archive_overlay(const TempDir& dir) {
  ++groups;
  const std::string archive = temp_path(dir, "resolver-src.tar.gz");
  CHECK_COUNTED(write_archive_entries(
      archive, {{"demo", '5', {}, {}}, {"demo/a.c", '0', "archive version\n", {}}}, true));

  const std::string overlay = temp_path(dir, "resolver-overlay");
  make_directory(join(overlay, "demo"));
  CHECK_COUNTED(write_text(join(overlay, "demo/a.c"), "overlay version\n"));

  const std::string cache = temp_path(dir, "resolver-cache");
  SourceResolver resolver(cache, true, false);
  const ModuleInfo module = make_module(archive, overlay);
  const SourceLocation location = resolver.map(module, "/build/demo", "/build/demo/a.c");
  CHECK_COUNTED(location.status == "resolved");
  CHECK_COUNTED(!location.resolved.empty());
  CHECK_COUNTED(read_text(location.resolved) == "overlay version\n");
  CHECK_COUNTED(location.resolved.find(cache) == 0);
  CHECK_COUNTED(!resolver.mappings().empty());
  if (!resolver.mappings().empty()) {
    CHECK_COUNTED(resolver.mappings().front().status == "resolved");
    CHECK_COUNTED(resolver.mappings().front().kind == "archive");
  }

  // A second lookup in the same extracted tree must resolve too.
  const SourceLocation second = resolver.map(module, "/build/demo", "/build/demo/a.c");
  CHECK_COUNTED(second.status == "resolved");
}

void test_source_resolver_directory_overlay(const TempDir& dir) {
  ++groups;
  const std::string source = temp_path(dir, "resolver-dir");
  make_directory(source);
  CHECK_COUNTED(write_text(join(source, "a.c"), "dir version\n"));

  const std::string overlay = temp_path(dir, "resolver-dir-overlay");
  make_directory(join(overlay, "demo"));
  CHECK_COUNTED(write_text(join(overlay, "demo/a.c"), "dir overlay version\n"));

  const std::string cache = temp_path(dir, "resolver-dir-cache");
  SourceResolver resolver(cache, true, false);
  const ModuleInfo module = make_module(source, overlay);
  const SourceLocation location = resolver.map(module, "/build/demo", "/build/demo/a.c");
  CHECK_COUNTED(location.status == "resolved");
  CHECK_COUNTED(read_text(location.resolved) == "dir overlay version\n");
}

void test_source_resolver_rejects_hostile_archive(const TempDir& dir) {
  ++groups;
  const std::string archive = temp_path(dir, "resolver-hostile.tar");
  const std::vector<unsigned char> bytes =
      build_tar({{"safe/a.c", '0', "ok\n", {}}, {"../escape.c", '0', "pwned\n", {}}});
  CHECK_COUNTED(write_bytes(archive, bytes.data(), bytes.size()));

  const std::string cache = temp_path(dir, "resolver-hostile-cache");
  SourceResolver resolver(cache, true, false);
  const ModuleInfo module = make_module(archive, "");
  const SourceLocation location = resolver.map(module, "/build/demo", "/build/demo/a.c");
  CHECK_COUNTED(location.status == "source-unsafe");
  CHECK_COUNTED(!path_exists(join(dir.path, "escape.c")));
}

void test_source_resolver_rejects_malformed_archive(const TempDir& dir) {
  ++groups;
  const std::string archive = temp_path(dir, "resolver-malformed.tar.gz");
  const std::vector<unsigned char> bytes = build_tar({{"demo/a.c", '0', "content\n", {}}});
  // Cut inside the first header: libarchive must report a damaged archive.
  CHECK_COUNTED(write_bytes(archive, bytes.data(), 100));
  const std::string cache = temp_path(dir, "resolver-malformed-cache");
  SourceResolver resolver(cache, true, false);
  const ModuleInfo module = make_module(archive, "");
  const SourceLocation location = resolver.map(module, "/build/demo", "/build/demo/a.c");
  CHECK_COUNTED(location.status == "source-unavailable");
}

} // namespace

int main() {
  std::cout << "archive_test\n";
  TempDir temp;
  if (temp.path.empty()) {
    std::cerr << "could not create a temporary directory\n";
    return 1;
  }

  test_safe_relative_path();
  test_valid_archive_extraction(temp);
  test_path_traversal_rejected(temp);
  test_symlink_entries(temp);
  test_symlink_write_through_rejected(temp);
  test_special_entries_skipped(temp);
  test_limits(temp);
  test_malformed_archives(temp);
  test_copy_directory_tree_and_overlay(temp);
  test_overlay_symlink_escape_rejected(temp);
  test_source_resolver_archive_overlay(temp);
  test_source_resolver_directory_overlay(temp);
  test_source_resolver_rejects_hostile_archive(temp);
  test_source_resolver_rejects_malformed_archive(temp);

  std::cout << "\n"
            << groups << " test groups, " << checks << " checks, " << nix_sym_test::failures
            << " failure(s)\n";
  return nix_sym_test::failures == 0 ? 0 : 1;
}
