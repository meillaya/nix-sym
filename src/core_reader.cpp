#include "core_reader.hpp"

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/procfs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "process.hpp"

namespace nix_sym {
namespace {

constexpr unsigned char kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};

std::string make_temp_path(const std::string& cache_root, const std::string& stem) {
  std::filesystem::path directory = cache_root;
  if (directory.empty()) {
    directory = std::filesystem::temp_directory_path();
  } else {
    directory /= "cores";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
      directory = std::filesystem::temp_directory_path();
    }
  }
  std::string pattern = (directory / (stem + "-XXXXXX")).string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  int fd = mkstemp(buffer.data());
  if (fd < 0) {
    throw std::runtime_error("cannot create temporary core file: " + std::string(strerror(errno)));
  }
  close(fd);
  return std::string(buffer.data());
}

void copy_fd(int input, int output) {
  char buffer[65536];
  while (true) {
    ssize_t n = read(input, buffer, sizeof(buffer));
    if (n > 0) {
      ssize_t written = 0;
      while (written < n) {
        ssize_t m = write(output, buffer + written, static_cast<size_t>(n - written));
        if (m < 0 && errno == EINTR) {
          continue;
        }
        if (m <= 0) {
          throw std::runtime_error("cannot write temporary core file");
        }
        written += m;
      }
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      throw std::runtime_error("cannot read core file: " + std::string(strerror(errno)));
    }
  }
}

bool fd_has_zstd_magic(int fd) {
  unsigned char bytes[4] = {0, 0, 0, 0};
  ssize_t count = pread(fd, bytes, sizeof(bytes), 0);
  return count == 4 && std::memcmp(bytes, kZstdMagic, 4) == 0;
}

bool has_zstd_magic(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  bool result = fd_has_zstd_magic(fd);
  close(fd);
  return result;
}

std::string byte_limit_message(const char* what, std::size_t value, std::size_t limit) {
  return std::string(what) + " exceeds the configured limit of " + std::to_string(limit) +
         " bytes (" + std::to_string(value) + " bytes)";
}

void write_all(int fd, const char* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    ssize_t n = write(fd, data + written, size - written);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      throw std::runtime_error("cannot write decompressed core");
    }
    written += static_cast<std::size_t>(n);
  }
}

// Streaming zstd decompression between two file descriptors. The output size is
// capped at max_output_bytes so a tiny compressed input can never make the
// parser allocate an unbounded temporary file.
void decompress_zstd_fd(int input_fd, int output_fd, std::size_t max_output_bytes) {
  if (lseek(input_fd, 0, SEEK_SET) < 0) {
    throw std::runtime_error("cannot seek compressed core: " + std::string(strerror(errno)));
  }

  ZSTD_DStream* stream = ZSTD_createDStream();
  if (stream == nullptr) {
    throw std::runtime_error("cannot allocate zstd stream");
  }
  ZSTD_initDStream(stream);

  std::vector<char> in_buffer(ZSTD_DStreamInSize());
  std::vector<char> out_buffer(ZSTD_DStreamOutSize());
  std::size_t total_output = 0;
  bool done = false;
  try {
    while (!done) {
      ssize_t read_count = read(input_fd, in_buffer.data(), in_buffer.size());
      if (read_count < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw std::runtime_error("cannot read compressed core: " + std::string(strerror(errno)));
      }
      if (read_count == 0) {
        break;
      }
      ZSTD_inBuffer zin{in_buffer.data(), static_cast<std::size_t>(read_count), 0};
      while (zin.pos < zin.size) {
        ZSTD_outBuffer zout{out_buffer.data(), out_buffer.size(), 0};
        std::size_t result = ZSTD_decompressStream(stream, &zout, &zin);
        if (ZSTD_isError(result)) {
          throw std::runtime_error("invalid zstd core: " + std::string(ZSTD_getErrorName(result)));
        }
        if (zout.pos > 0) {
          if (total_output > max_output_bytes || zout.pos > max_output_bytes - total_output) {
            throw std::runtime_error(
                byte_limit_message("decompressed core", total_output + zout.pos, max_output_bytes));
          }
          write_all(output_fd, out_buffer.data(), zout.pos);
          total_output += zout.pos;
        }
        if (result == 0) {
          done = true;
          break;
        }
      }
    }
  } catch (...) {
    ZSTD_freeDStream(stream);
    throw;
  }
  ZSTD_freeDStream(stream);
  if (!done) {
    throw std::runtime_error("truncated zstd core");
  }
}

void decompress_zstd(const std::string& input_path, const std::string& output_path,
                     std::size_t max_output_bytes) {
  int input_fd = open(input_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (input_fd < 0) {
    throw std::runtime_error("cannot open compressed core: " + input_path);
  }
  int output_fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (output_fd < 0) {
    close(input_fd);
    throw std::runtime_error("cannot open output core: " + std::string(strerror(errno)));
  }
  try {
    decompress_zstd_fd(input_fd, output_fd, max_output_bytes);
  } catch (...) {
    close(input_fd);
    close(output_fd);
    std::error_code ec;
    std::filesystem::remove(output_path, ec);
    throw;
  }
  close(input_fd);
  close(output_fd);
}

std::string read_stdin_to_path(const std::string& cache_root) {
  std::string path = make_temp_path(cache_root, "stdin-core");
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    throw std::runtime_error("cannot create stdin core file");
  }
  try {
    copy_fd(STDIN_FILENO, fd);
  } catch (...) {
    close(fd);
    std::filesystem::remove(path);
    throw;
  }
  close(fd);
  return path;
}

std::string run_coredumpctl(const std::string& pid, const std::string& cache_root) {
  std::string path = make_temp_path(cache_root, "coredumpctl-core");
  std::vector<std::string> argv = {"coredumpctl", "dump", "--output", path, pid};
  ProcessResult result = run_process(argv, 120);
  if (!result.started || result.timed_out || result.exit_code != 0) {
    std::filesystem::remove(path);
    std::string detail = result.err.empty() ? result.out : result.err;
    throw std::runtime_error("coredumpctl dump failed" +
                             (detail.empty() ? std::string() : ": " + detail));
  }
  return path;
}

std::string note_name(Elf_Data* data, std::size_t name_offset, std::size_t name_size) {
  if (data == nullptr || data->d_buf == nullptr || name_offset > data->d_size ||
      name_size > data->d_size - name_offset) {
    return {};
  }
  return std::string(static_cast<const char*>(data->d_buf) + name_offset, name_size);
}

void parse_notes(Elf* elf, CoreInfo* info, const CoreLimits& limits) {
  if (elf == nullptr || info == nullptr) {
    return;
  }
  std::size_t phnum = 0;
  if (elf_getphdrnum(elf, &phnum) != 0) {
    return;
  }
  std::size_t notes_parsed = 0;
  for (std::size_t i = 0; i < phnum; ++i) {
    GElf_Phdr phdr;
    if (gelf_getphdr(elf, i, &phdr) == nullptr || phdr.p_type != PT_NOTE) {
      continue;
    }
    if (phdr.p_filesz == 0) {
      continue;
    }
    if (phdr.p_filesz > limits.max_note_bytes) {
      throw std::runtime_error(
          byte_limit_message("core note segment", phdr.p_filesz, limits.max_note_bytes));
    }
    if (phdr.p_offset > static_cast<Elf64_Off>(std::numeric_limits<off_t>::max())) {
      throw std::runtime_error("core note segment offset is out of range");
    }
    Elf_Data* data =
        elf_getdata_rawchunk(elf, static_cast<off_t>(phdr.p_offset), phdr.p_filesz, ELF_T_NHDR);
    if (data == nullptr || data->d_buf == nullptr || data->d_size == 0) {
      // A truncated note segment cannot be read; ignore it and keep parsing the
      // remaining program headers.
      continue;
    }
    if (data->d_size > limits.max_note_bytes) {
      throw std::runtime_error(
          byte_limit_message("core note segment", data->d_size, limits.max_note_bytes));
    }
    std::size_t offset = 0;
    GElf_Nhdr nhdr;
    std::size_t name_offset = 0;
    std::size_t desc_offset = 0;
    while (true) {
      std::size_t next = gelf_getnote(data, offset, &nhdr, &name_offset, &desc_offset);
      if (next == 0) {
        break;
      }
      offset = next;
      if (notes_parsed >= limits.max_notes) {
        return;
      }
      ++notes_parsed;
      if (nhdr.n_descsz > limits.max_note_bytes) {
        // An absurd descriptor size cannot fit in the bounded segment; stop
        // parsing instead of trying to allocate it.
        return;
      }
      if (desc_offset > data->d_size || nhdr.n_descsz > data->d_size - desc_offset) {
        break;
      }
      const auto* desc = static_cast<const unsigned char*>(data->d_buf) + desc_offset;
      std::string name = note_name(data, name_offset, nhdr.n_namesz);
      if (name.find("CORE") == std::string::npos) {
        continue;
      }
      if (nhdr.n_type == NT_PRPSINFO && nhdr.n_descsz >= sizeof(prpsinfo_t)) {
        prpsinfo_t ps;
        std::memcpy(&ps, desc, sizeof(ps));
        if (info->pid <= 0) {
          info->pid = ps.pr_pid;
        }
        if (info->exe.empty()) {
          info->exe.assign(ps.pr_fname, strnlen(ps.pr_fname, sizeof(ps.pr_fname)));
        }
      } else if (nhdr.n_type == NT_SIGINFO && nhdr.n_descsz >= sizeof(siginfo_t)) {
        siginfo_t si;
        std::memcpy(&si, desc, sizeof(si));
        info->signal = si.si_signo;
        if (info->signal > 0) {
          const char* abbreviation = sigabbrev_np(info->signal);
          if (abbreviation != nullptr) {
            info->signal_name = std::string("SIG") + abbreviation;
          } else {
            info->signal_name = "SIG" + std::to_string(info->signal);
          }
        }
      }
    }
  }
}

void close_core(CoreFile* core) {
  if (core->elf != nullptr) {
    elf_end(core->elf);
    core->elf = nullptr;
  }
  if (core->fd >= 0) {
    close(core->fd);
    core->fd = -1;
  }
}

class ElfCloser {
public:
  explicit ElfCloser(Elf* elf) : elf_(elf) {}
  ~ElfCloser() {
    if (elf_ != nullptr) {
      elf_end(elf_);
    }
  }
  ElfCloser(const ElfCloser&) = delete;
  ElfCloser& operator=(const ElfCloser&) = delete;
  Elf* get() const {
    return elf_;
  }

private:
  Elf* elf_ = nullptr;
};

// Opens and validates an ELF core image. Returns an elf handle owned by the
// caller, or throws with a descriptive message.
Elf* begin_elf_core(int fd, const CoreLimits& limits) {
  struct stat st;
  if (fstat(fd, &st) != 0) {
    throw std::runtime_error("cannot stat core file: " + std::string(strerror(errno)));
  }
  if (st.st_size < 0) {
    throw std::runtime_error("cannot stat core file: negative size");
  }
  const std::size_t size = static_cast<std::size_t>(st.st_size);
  if (size > limits.max_file_bytes) {
    throw std::runtime_error(byte_limit_message("core file", size, limits.max_file_bytes));
  }
  elf_version(EV_CURRENT);
  Elf* elf = elf_begin(fd, ELF_C_READ_MMAP, nullptr);
  if (elf == nullptr) {
    throw std::runtime_error("not an ELF core file: " + std::string(elf_errmsg(-1)));
  }
  if (elf_kind(elf) != ELF_K_ELF) {
    elf_end(elf);
    throw std::runtime_error("not an ELF core file");
  }
  GElf_Ehdr ehdr;
  if (gelf_getehdr(elf, &ehdr) == nullptr || ehdr.e_type != ET_CORE) {
    elf_end(elf);
    throw std::runtime_error("ELF file is not a core dump");
  }
  return elf;
}

CoreInfo inspect_core_fd(int fd, const CoreLimits& limits) {
  ElfCloser elf(begin_elf_core(fd, limits));
  CoreInfo info;
  info.elf = true;
  parse_notes(elf.get(), &info, limits);
  return info;
}

int create_anonymous_fd() {
  int fd = memfd_create("nix-sym-core", MFD_CLOEXEC);
  if (fd < 0) {
    throw std::runtime_error("cannot create in-memory core buffer: " +
                             std::string(strerror(errno)));
  }
  return fd;
}

} // namespace

CoreFile::CoreFile(CoreFile&& other) noexcept
    : path(std::move(other.path)), temp_path(std::move(other.temp_path)),
      compressed(other.compressed), fd(other.fd), elf(other.elf) {
  other.fd = -1;
  other.elf = nullptr;
  other.temp_path.clear();
}

CoreFile& CoreFile::operator=(CoreFile&& other) noexcept {
  if (this != &other) {
    close_core(this);
    if (!temp_path.empty()) {
      std::filesystem::remove(temp_path);
    }
    path = std::move(other.path);
    temp_path = std::move(other.temp_path);
    compressed = other.compressed;
    fd = other.fd;
    elf = other.elf;
    other.fd = -1;
    other.elf = nullptr;
    other.temp_path.clear();
  }
  return *this;
}

CoreFile::~CoreFile() {
  close_core(this);
  if (!temp_path.empty()) {
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
  }
}

CoreFile open_core_file(const std::string& path, const std::string& cache_root,
                        const std::string& coredumpctl_pid, const CoreLimits& limits) {
  CoreFile core;
  std::string source = path;
  if (!coredumpctl_pid.empty()) {
    core.temp_path = run_coredumpctl(coredumpctl_pid, cache_root);
    source = core.temp_path;
  } else if (path == "-") {
    core.temp_path = read_stdin_to_path(cache_root);
    source = core.temp_path;
  } else {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      throw std::runtime_error("core file not found: " + path);
    }
  }

  core.compressed = has_zstd_magic(source);
  std::string opened_path = source;
  if (core.compressed) {
    std::string compressed_source = source;
    std::string decompressed = make_temp_path(cache_root, "decompressed-core");
    decompress_zstd(source, decompressed, limits.max_decompressed_bytes);
    if (!core.temp_path.empty() && core.temp_path != compressed_source) {
      std::error_code ec;
      std::filesystem::remove(core.temp_path, ec);
    }
    core.temp_path = decompressed;
    source = decompressed;
    opened_path = decompressed;
  }

  core.path = !coredumpctl_pid.empty() ? ("coredumpctl:" + coredumpctl_pid) : path;
  core.fd = open(opened_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (core.fd < 0) {
    throw std::runtime_error("cannot open core: " + std::string(strerror(errno)));
  }
  core.elf = begin_elf_core(core.fd, limits);
  return core;
}

CoreInfo read_core_info(const CoreFile& core, const CoreLimits& limits) {
  CoreInfo info;
  info.path = core.path;
  info.compressed = core.compressed;
  info.elf = core.elf != nullptr;
  parse_notes(core.elf, &info, limits);
  return info;
}

CoreInfo read_core_info_fd(int fd, const CoreLimits& limits) {
  if (fd < 0) {
    throw std::runtime_error("invalid core file descriptor");
  }
  if (!fd_has_zstd_magic(fd)) {
    return inspect_core_fd(fd, limits);
  }

  int decompressed_fd = create_anonymous_fd();
  try {
    decompress_zstd_fd(fd, decompressed_fd, limits.max_decompressed_bytes);
    if (lseek(decompressed_fd, 0, SEEK_SET) < 0) {
      throw std::runtime_error("cannot rewind decompressed core: " + std::string(strerror(errno)));
    }
    CoreInfo info = inspect_core_fd(decompressed_fd, limits);
    info.compressed = true;
    close(decompressed_fd);
    return info;
  } catch (...) {
    close(decompressed_fd);
    throw;
  }
}

} // namespace nix_sym
