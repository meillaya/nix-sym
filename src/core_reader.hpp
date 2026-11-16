#pragma once

#include <elfutils/libdwfl.h>
#include <gelf.h>

#include <string>

#include "model.hpp"

namespace nix_sym {

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
                        const std::string& coredumpctl_pid);
CoreInfo read_core_info(const CoreFile& core);

} // namespace nix_sym
