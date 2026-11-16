#include "core_reader.hpp"

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/procfs.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
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

bool has_zstd_magic(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  unsigned char bytes[4] = {0, 0, 0, 0};
  input.read(reinterpret_cast<char*>(bytes), 4);
  return input.gcount() == 4 && std::memcmp(bytes, kZstdMagic, 4) == 0;
}

void decompress_zstd(const std::string& input_path, const std::string& output_path) {
  std::ifstream input(input_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open compressed core: " + input_path);
  }
  int output_fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (output_fd < 0) {
    throw std::runtime_error("cannot open output core: " + std::string(strerror(errno)));
  }

  ZSTD_DStream* stream = ZSTD_createDStream();
  if (stream == nullptr) {
    close(output_fd);
    throw std::runtime_error("cannot allocate zstd stream");
  }
  ZSTD_initDStream(stream);

  std::vector<char> in_buffer(ZSTD_DStreamInSize());
  std::vector<char> out_buffer(ZSTD_DStreamOutSize());
  bool done = false;
  try {
    while (input && !done) {
      input.read(in_buffer.data(), static_cast<std::streamsize>(in_buffer.size()));
      std::streamsize read_count = input.gcount();
      ZSTD_inBuffer zin{in_buffer.data(), static_cast<size_t>(read_count), 0};
      while (zin.pos < zin.size) {
        ZSTD_outBuffer zout{out_buffer.data(), out_buffer.size(), 0};
        size_t result = ZSTD_decompressStream(stream, &zout, &zin);
        if (ZSTD_isError(result)) {
          throw std::runtime_error("invalid zstd core: " + std::string(ZSTD_getErrorName(result)));
        }
        size_t written = 0;
        while (written < zout.pos) {
          ssize_t n = write(output_fd, out_buffer.data() + written, zout.pos - written);
          if (n < 0 && errno == EINTR) {
            continue;
          }
          if (n <= 0) {
            throw std::runtime_error("cannot write decompressed core");
          }
          written += static_cast<size_t>(n);
        }
        if (result == 0) {
          done = true;
          break;
        }
      }
    }
  } catch (...) {
    ZSTD_freeDStream(stream);
    close(output_fd);
    std::filesystem::remove(output_path);
    throw;
  }
  ZSTD_freeDStream(stream);
  close(output_fd);
  if (!done) {
    throw std::runtime_error("truncated zstd core");
  }
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

std::string note_name(Elf_Data* data, size_t name_offset, size_t name_size) {
  if (data == nullptr || data->d_buf == nullptr || name_offset + name_size > data->d_size) {
    return {};
  }
  return std::string(static_cast<const char*>(data->d_buf) + name_offset, name_size);
}

void parse_notes(Elf* elf, CoreInfo* info) {
  size_t phnum = 0;
  if (elf_getphdrnum(elf, &phnum) != 0) {
    return;
  }
  for (size_t i = 0; i < phnum; ++i) {
    GElf_Phdr phdr;
    if (gelf_getphdr(elf, i, &phdr) == nullptr || phdr.p_type != PT_NOTE) {
      continue;
    }
    Elf_Data* data =
        elf_getdata_rawchunk(elf, static_cast<off_t>(phdr.p_offset), phdr.p_filesz, ELF_T_NHDR);
    if (data == nullptr) {
      continue;
    }
    size_t offset = 0;
    GElf_Nhdr nhdr;
    size_t name_offset = 0;
    size_t desc_offset = 0;
    while ((offset = gelf_getnote(data, offset, &nhdr, &name_offset, &desc_offset)) > 0) {
      if (desc_offset + nhdr.n_descsz > data->d_size) {
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
                        const std::string& coredumpctl_pid) {
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
    decompress_zstd(source, decompressed);
    if (!core.temp_path.empty() && core.temp_path != compressed_source) {
      std::error_code ec;
      std::filesystem::remove(core.temp_path, ec);
    }
    core.temp_path = decompressed;
    source = decompressed;
    opened_path = decompressed;
  }

  core.path = !coredumpctl_pid.empty() ? ("coredumpctl:" + coredumpctl_pid) : path;
  core.fd = open(opened_path.c_str(), O_RDONLY);
  if (core.fd < 0) {
    throw std::runtime_error("cannot open core: " + std::string(strerror(errno)));
  }
  elf_version(EV_CURRENT);
  core.elf = elf_begin(core.fd, ELF_C_READ_MMAP, nullptr);
  if (core.elf == nullptr) {
    throw std::runtime_error("not an ELF core file: " + std::string(elf_errmsg(-1)));
  }
  if (elf_kind(core.elf) != ELF_K_ELF) {
    throw std::runtime_error("not an ELF core file");
  }
  GElf_Ehdr ehdr;
  if (gelf_getehdr(core.elf, &ehdr) == nullptr || ehdr.e_type != ET_CORE) {
    throw std::runtime_error("ELF file is not a core dump");
  }
  return core;
}

CoreInfo read_core_info(const CoreFile& core) {
  CoreInfo info;
  info.path = core.path;
  info.compressed = core.compressed;
  parse_notes(core.elf, &info);
  return info;
}

} // namespace nix_sym
