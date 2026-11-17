#include "../test.hpp"

#include "core_reader.hpp"

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <unistd.h>
#include <zstd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using nix_sym::CoreFile;
using nix_sym::CoreInfo;
using nix_sym::CoreLimits;

int checks = 0;
int groups = 0;

#define CHECK_COUNTED(expr)                                                                        \
  do {                                                                                             \
    ++checks;                                                                                      \
    CHECK(expr);                                                                                   \
  } while (false)

std::string expect_throws(const char* what, const std::function<void()>& action) {
  try {
    action();
  } catch (const std::exception& error) {
    return error.what();
  }
  nix_sym_test::fail(__FILE__, __LINE__, std::string("expected exception from ") + what);
  return {};
}

#define EXPECT_THROWS(what, ...)                                                                   \
  ([&]() -> std::string {                                                                          \
    ++checks;                                                                                      \
    return expect_throws(what, __VA_ARGS__);                                                       \
  })()

// ---------------------------------------------------------------------------
// Minimal ELF core builder
// ---------------------------------------------------------------------------

constexpr std::uint16_t kCorePid = 4242;

void append_bytes(std::vector<unsigned char>* image, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  image->insert(image->end(), bytes, bytes + size);
}

void align_to_four(std::vector<unsigned char>* image) {
  while (image->size() % 4 != 0) {
    image->push_back(0);
  }
}

void append_note(std::vector<unsigned char>* notes, std::uint32_t type, const std::string& name,
                 const void* desc, std::size_t desc_size) {
  align_to_four(notes);
  Elf64_Nhdr header{};
  header.n_namesz = static_cast<std::uint32_t>(name.size() + 1);
  header.n_descsz = static_cast<std::uint32_t>(desc_size);
  header.n_type = type;
  append_bytes(notes, &header, sizeof(header));
  append_bytes(notes, name.c_str(), name.size() + 1);
  align_to_four(notes);
  append_bytes(notes, desc, desc_size);
  align_to_four(notes);
}

std::vector<unsigned char> default_notes() {
  std::vector<unsigned char> notes;
  prpsinfo_t ps;
  std::memset(&ps, 0, sizeof(ps));
  ps.pr_pid = kCorePid;
  const char* name = "core-test";
  std::memcpy(ps.pr_fname, name, std::strlen(name));
  append_note(&notes, NT_PRPSINFO, "CORE", &ps, sizeof(ps));

  siginfo_t si;
  std::memset(&si, 0, sizeof(si));
  si.si_signo = SIGSEGV;
  append_note(&notes, NT_SIGINFO, "CORE", &si, sizeof(si));
  return notes;
}

// Builds a minimal 64-bit ET_CORE (or other ELF type) image with one PT_NOTE
// program header pointing at notes.
std::vector<unsigned char> make_core_image(const std::vector<unsigned char>& notes,
                                           std::uint16_t elf_type = ET_CORE,
                                           bool truncate_note_segment = false,
                                           std::uint64_t extra_note_bytes = 0) {
  std::vector<unsigned char> image(sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr), 0);
  Elf64_Ehdr header{};
  std::memcpy(header.e_ident, ELFMAG, SELFMAG);
  header.e_ident[EI_CLASS] = ELFCLASS64;
  header.e_ident[EI_DATA] = ELFDATA2LSB;
  header.e_ident[EI_VERSION] = EV_CURRENT;
  header.e_ident[EI_OSABI] = ELFOSABI_NONE;
  header.e_type = elf_type;
  header.e_machine = EM_X86_64;
  header.e_version = EV_CURRENT;
  header.e_phoff = sizeof(Elf64_Ehdr);
  header.e_ehsize = sizeof(Elf64_Ehdr);
  header.e_phentsize = sizeof(Elf64_Phdr);
  header.e_phnum = 1;
  std::memcpy(image.data(), &header, sizeof(header));

  Elf64_Phdr program_header{};
  program_header.p_type = PT_NOTE;
  program_header.p_flags = PF_R;
  program_header.p_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
  program_header.p_filesz = notes.size() + extra_note_bytes;
  program_header.p_memsz = notes.size() + extra_note_bytes;
  program_header.p_align = 4;
  std::memcpy(image.data() + sizeof(Elf64_Ehdr), &program_header, sizeof(program_header));

  if (!truncate_note_segment) {
    image.insert(image.end(), notes.begin(), notes.end());
  } else {
    // The program header promises more bytes than the file carries.
    image.insert(image.end(), notes.begin(), notes.begin() + notes.size() / 2);
  }
  return image;
}

// Overwrites the advertised note segment size without extending the file, as a
// truncated-core producer might.
std::vector<unsigned char> make_core_with_huge_note_segment(std::size_t note_segment_size) {
  std::vector<unsigned char> image = make_core_image(default_notes());
  Elf64_Phdr program_header{};
  std::memcpy(&program_header, image.data() + sizeof(Elf64_Ehdr), sizeof(program_header));
  program_header.p_filesz = note_segment_size;
  program_header.p_memsz = note_segment_size;
  std::memcpy(image.data() + sizeof(Elf64_Ehdr), &program_header, sizeof(program_header));
  return image;
}

// A note whose descriptor claims to be larger than the containing segment.
std::vector<unsigned char> truncated_note_payload() {
  std::vector<unsigned char> notes;
  const std::string name = "CORE";
  Elf64_Nhdr header{};
  header.n_namesz = static_cast<std::uint32_t>(name.size() + 1);
  header.n_descsz = 4096; // absurd: only a few bytes actually follow
  header.n_type = NT_PRPSINFO;
  append_bytes(&notes, &header, sizeof(header));
  append_bytes(&notes, name.c_str(), name.size() + 1);
  align_to_four(&notes);
  append_bytes(&notes, "short", 5);
  align_to_four(&notes);
  return make_core_image(notes);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int create_memfd(const std::string& name) {
  int fd = memfd_create(name.c_str(), MFD_CLOEXEC);
  return fd;
}

int memfd_with_bytes(const std::vector<unsigned char>& bytes) {
  int fd = create_memfd("nix-sym-core-test");
  if (fd < 0) {
    return -1;
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    ssize_t n = write(fd, bytes.data() + written, bytes.size() - written);
    if (n <= 0) {
      close(fd);
      return -1;
    }
    written += static_cast<std::size_t>(n);
  }
  if (lseek(fd, 0, SEEK_SET) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

std::vector<unsigned char> zstd_compress(const std::vector<unsigned char>& input) {
  std::vector<unsigned char> output(ZSTD_compressBound(input.size()));
  std::size_t size = ZSTD_compress(output.data(), output.size(), input.data(), input.size(), 3);
  if (ZSTD_isError(size)) {
    return {};
  }
  output.resize(size);
  return output;
}

struct TempDir {
  std::string path;

  TempDir() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
      return;
    }
    std::string pattern = (base / "nix-sym-core-test-XXXXXX").string();
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
      std::filesystem::remove_all(path, ec);
    }
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

bool write_file(const std::string& path, const std::vector<unsigned char>& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(content.data()),
            static_cast<std::streamsize>(content.size()));
  return static_cast<bool>(out);
}

std::string path_join(const std::string& directory, const std::string& name) {
  return (std::filesystem::path(directory) / name).string();
}

CoreLimits tight_limits() {
  CoreLimits limits;
  limits.max_file_bytes = 1024 * 1024;
  limits.max_decompressed_bytes = 1024 * 1024;
  limits.max_note_bytes = 64 * 1024;
  limits.max_notes = 1024;
  return limits;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_invalid_fd() {
  ++groups;
  const std::string message =
      EXPECT_THROWS("read_core_info_fd(-1)", [] { (void)nix_sym::read_core_info_fd(-1); });
  CHECK_COUNTED(message.find("invalid core file descriptor") != std::string::npos);
}

void test_empty_inputs(const TempDir& dir) {
  ++groups;
  // Empty file descriptor.
  int fd = create_memfd("nix-sym-empty");
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    const std::string message =
        EXPECT_THROWS("empty fd", [fd] { (void)nix_sym::read_core_info_fd(fd); });
    CHECK_COUNTED(!message.empty());
    close(fd);
  }

  // Empty file on disk.
  const std::string path = path_join(dir.path, "empty.core");
  CHECK_COUNTED(write_file(path, {}));
  const std::string message = EXPECT_THROWS(
      "empty file", [&path, &dir] { (void)nix_sym::open_core_file(path, dir.path, ""); });
  CHECK_COUNTED(!message.empty());
}

void test_non_elf_inputs() {
  ++groups;
  const std::vector<unsigned char> junk = {'n', 'o', 't', ' ', 'e', 'l', 'f', '\n'};
  int fd = memfd_with_bytes(junk);
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    const std::string message =
        EXPECT_THROWS("junk fd", [fd] { (void)nix_sym::read_core_info_fd(fd); });
    CHECK_COUNTED(message.find("not an ELF") != std::string::npos);
    close(fd);
  }
}

void test_truncated_elf() {
  ++groups;
  const std::vector<unsigned char> image = make_core_image(default_notes());
  // Cut in the middle of the ELF header.
  const std::vector<unsigned char> truncated(image.begin(), image.begin() + 20);
  int fd = memfd_with_bytes(truncated);
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    const std::string message =
        EXPECT_THROWS("truncated ELF", [fd] { (void)nix_sym::read_core_info_fd(fd); });
    CHECK_COUNTED(!message.empty());
    close(fd);
  }
}

void test_elf_not_core(const TempDir& dir) {
  ++groups;
  const std::vector<unsigned char> image = make_core_image(default_notes(), ET_EXEC);
  int fd = memfd_with_bytes(image);
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    const std::string message =
        EXPECT_THROWS("ET_EXEC", [fd] { (void)nix_sym::read_core_info_fd(fd); });
    CHECK_COUNTED(message.find("not a core") != std::string::npos);
    close(fd);
  }

  const std::string path = path_join(dir.path, "exec.elf");
  CHECK_COUNTED(write_file(path, image));
  const std::string message = EXPECT_THROWS(
      "ET_EXEC file", [&path, &dir] { (void)nix_sym::open_core_file(path, dir.path, ""); });
  CHECK_COUNTED(message.find("not a core") != std::string::npos);
}

void test_notes_from_fd() {
  ++groups;
  const std::vector<unsigned char> image = make_core_image(default_notes());
  int fd = memfd_with_bytes(image);
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    CoreInfo info;
    try {
      info = nix_sym::read_core_info_fd(fd, tight_limits());
    } catch (const std::exception& error) {
      nix_sym_test::fail(__FILE__, __LINE__,
                         std::string("read_core_info_fd threw: ") + error.what());
    }
    CHECK_COUNTED(info.pid == static_cast<int>(kCorePid));
    CHECK_COUNTED(info.exe == "core-test");
    CHECK_COUNTED(info.signal == SIGSEGV);
    CHECK_COUNTED(info.signal_name == "SIGSEGV");
    CHECK_COUNTED(info.elf);
    CHECK_COUNTED(!info.compressed);
    // The parser must not take ownership of the descriptor.
    CHECK_COUNTED(lseek(fd, 0, SEEK_SET) == 0);
    close(fd);
  }
}

void test_notes_from_file(const TempDir& dir) {
  ++groups;
  const std::vector<unsigned char> image = make_core_image(default_notes());
  const std::string path = path_join(dir.path, "valid.core");
  CHECK_COUNTED(write_file(path, image));
  CoreInfo info;
  try {
    CoreFile core = nix_sym::open_core_file(path, dir.path, "");
    CHECK_COUNTED(core.compressed == false);
    CHECK_COUNTED(core.valid());
    info = nix_sym::read_core_info(core);
  } catch (const std::exception& error) {
    nix_sym_test::fail(__FILE__, __LINE__,
                       std::string("open/read_core_info threw: ") + error.what());
  }
  CHECK_COUNTED(info.path == path);
  CHECK_COUNTED(info.pid == static_cast<int>(kCorePid));
  CHECK_COUNTED(info.signal_name == "SIGSEGV");
}

void test_truncated_note_payload() {
  ++groups;
  // Note descriptor claims far more bytes than the segment carries.
  {
    const std::vector<unsigned char> image = truncated_note_payload();
    int fd = memfd_with_bytes(image);
    CHECK_COUNTED(fd >= 0);
    if (fd >= 0) {
      CoreInfo info;
      bool threw = false;
      try {
        info = nix_sym::read_core_info_fd(fd, tight_limits());
      } catch (const std::exception&) {
        threw = true;
      }
      // Truncated payload must be ignored, not crash or over-read.
      CHECK_COUNTED(!threw);
      CHECK_COUNTED(info.pid <= 0);
      close(fd);
    }
  }

  // Program header promises a note segment longer than the file.
  {
    const std::vector<unsigned char> image = make_core_image(default_notes(), ET_CORE, true);
    int fd = memfd_with_bytes(image);
    CHECK_COUNTED(fd >= 0);
    if (fd >= 0) {
      CoreInfo info;
      bool threw = false;
      try {
        info = nix_sym::read_core_info_fd(fd, tight_limits());
      } catch (const std::exception&) {
        threw = true;
      }
      CHECK_COUNTED(!threw);
      CHECK_COUNTED(info.pid <= 0);
      close(fd);
    }
  }
}

void test_absurd_note_sizes(const TempDir& dir) {
  ++groups;
  CoreLimits limits = tight_limits();
  limits.max_note_bytes = 1024;
  const std::vector<unsigned char> image = make_core_with_huge_note_segment(1024 * 1024);
  int fd = memfd_with_bytes(image);
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    const std::string message = EXPECT_THROWS(
        "huge note segment", [fd, &limits] { (void)nix_sym::read_core_info_fd(fd, limits); });
    CHECK_COUNTED(message.find("note segment") != std::string::npos);
    close(fd);
  }

  const std::string path = path_join(dir.path, "huge-note.core");
  CHECK_COUNTED(write_file(path, image));
  try {
    CoreFile core = nix_sym::open_core_file(path, dir.path, "");
    const std::string message = EXPECT_THROWS("read_core_info huge note", [&core, &limits] {
      (void)nix_sym::read_core_info(core, limits);
    });
    CHECK_COUNTED(message.find("note segment") != std::string::npos);
  } catch (const std::exception& error) {
    nix_sym_test::fail(__FILE__, __LINE__,
                       std::string("open_core_file threw on huge note: ") + error.what());
  }
}

void test_file_size_limit() {
  ++groups;
  CoreLimits limits = tight_limits();
  limits.max_file_bytes = 16;
  const std::vector<unsigned char> image = make_core_image(default_notes());
  int fd = memfd_with_bytes(image);
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    const std::string message = EXPECT_THROWS(
        "file too large", [fd, &limits] { (void)nix_sym::read_core_info_fd(fd, limits); });
    CHECK_COUNTED(message.find("core file exceeds") != std::string::npos);
    close(fd);
  }
}

void test_zstd_inputs(const TempDir& dir) {
  ++groups;
  const std::vector<unsigned char> image = make_core_image(default_notes());
  const std::vector<unsigned char> compressed = zstd_compress(image);
  CHECK_COUNTED(!compressed.empty());

  // Compressed core through the fd parser (memfd, no filesystem involved).
  {
    int fd = memfd_with_bytes(compressed);
    CHECK_COUNTED(fd >= 0);
    if (fd >= 0) {
      CoreInfo info;
      bool threw = false;
      try {
        info = nix_sym::read_core_info_fd(fd, tight_limits());
      } catch (const std::exception& error) {
        threw = true;
        nix_sym_test::fail(__FILE__, __LINE__, std::string("compressed fd threw: ") + error.what());
      }
      CHECK_COUNTED(!threw);
      CHECK_COUNTED(info.compressed);
      CHECK_COUNTED(info.pid == static_cast<int>(kCorePid));
      CHECK_COUNTED(info.signal_name == "SIGSEGV");
      close(fd);
    }
  }

  // Compressed core through open_core_file.
  {
    const std::string path = path_join(dir.path, "valid.core.zst");
    CHECK_COUNTED(write_file(path, compressed));
    bool threw = false;
    try {
      CoreFile core = nix_sym::open_core_file(path, dir.path, "");
      CHECK_COUNTED(core.compressed);
      const CoreInfo info = nix_sym::read_core_info(core);
      CHECK_COUNTED(info.compressed);
      CHECK_COUNTED(info.pid == static_cast<int>(kCorePid));
      CHECK_COUNTED(info.path == path);
    } catch (const std::exception& error) {
      threw = true;
      nix_sym_test::fail(__FILE__, __LINE__, std::string("compressed file threw: ") + error.what());
    }
    CHECK_COUNTED(!threw);
  }

  // Invalid zstd frame (magic followed by garbage).
  {
    std::vector<unsigned char> invalid = {0x28, 0xB5, 0x2F, 0xFD, 0xFF, 0xFF, 0x00, 0x42, 0x13};
    const std::string path = path_join(dir.path, "invalid.core.zst");
    CHECK_COUNTED(write_file(path, invalid));
    const std::string message = EXPECT_THROWS(
        "invalid zstd", [&path, &dir] { (void)nix_sym::open_core_file(path, dir.path, ""); });
    CHECK_COUNTED(message.find("zstd") != std::string::npos);
  }
  {
    int fd = memfd_with_bytes({0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00, 0x00, 0x00});
    CHECK_COUNTED(fd >= 0);
    if (fd >= 0) {
      const std::string message =
          EXPECT_THROWS("invalid zstd fd", [fd] { (void)nix_sym::read_core_info_fd(fd); });
      CHECK_COUNTED(message.find("zstd") != std::string::npos);
      close(fd);
    }
  }

  // Truncated zstd frame.
  {
    std::vector<unsigned char> truncated = compressed;
    truncated.resize(truncated.size() > 4 ? truncated.size() - 4 : 0);
    int fd = memfd_with_bytes(truncated);
    CHECK_COUNTED(fd >= 0);
    if (fd >= 0) {
      const std::string message =
          EXPECT_THROWS("truncated zstd", [fd] { (void)nix_sym::read_core_info_fd(fd); });
      CHECK_COUNTED(message.find("zstd") != std::string::npos);
      close(fd);
    }
  }
}

void test_decompression_limit() {
  ++groups;
  // A small, highly compressible image. With a tiny decompressed-size cap the
  // parser must abort instead of expanding it fully.
  std::vector<unsigned char> image = make_core_image(default_notes());
  image.resize(256 * 1024, 0); // still a valid-ish ELF, but far above the cap
  const std::vector<unsigned char> compressed = zstd_compress(image);
  CHECK_COUNTED(!compressed.empty());
  int fd = memfd_with_bytes(compressed);
  CHECK_COUNTED(fd >= 0);
  if (fd >= 0) {
    CoreLimits limits = tight_limits();
    limits.max_decompressed_bytes = 1024;
    const std::string message = EXPECT_THROWS(
        "decompression bomb", [fd, &limits] { (void)nix_sym::read_core_info_fd(fd, limits); });
    CHECK_COUNTED(message.find("decompressed core exceeds") != std::string::npos);
    close(fd);
  }

  const std::vector<unsigned char> non_elf = {'h', 'e', 'l', 'l', 'o'};
  const std::vector<unsigned char> compressed_non_elf = zstd_compress(non_elf);
  int non_elf_fd = memfd_with_bytes(compressed_non_elf);
  CHECK_COUNTED(non_elf_fd >= 0);
  if (non_elf_fd >= 0) {
    const std::string message = EXPECT_THROWS(
        "compressed non-ELF", [non_elf_fd] { (void)nix_sym::read_core_info_fd(non_elf_fd); });
    CHECK_COUNTED(message.find("not an ELF") != std::string::npos);
    close(non_elf_fd);
  }
}

} // namespace

int main() {
  std::cout << "core_reader_test\n";
  TempDir temp;
  if (temp.path.empty()) {
    std::cerr << "could not create a temporary directory\n";
    return 1;
  }

  test_invalid_fd();
  test_empty_inputs(temp);
  test_non_elf_inputs();
  test_truncated_elf();
  test_elf_not_core(temp);
  test_notes_from_fd();
  test_notes_from_file(temp);
  test_truncated_note_payload();
  test_absurd_note_sizes(temp);
  test_file_size_limit();
  test_zstd_inputs(temp);
  test_decompression_limit();

  std::cout << "\n"
            << groups << " test groups, " << checks << " checks, " << nix_sym_test::failures
            << " failure(s)\n";
  return nix_sym_test::failures == 0 ? 0 : 1;
}
