#pragma once

#include <string>
#include <vector>

#include "model.hpp"

namespace nix_sym {

struct DebugResolution {
  ModuleStatus status = ModuleStatus::Unknown;
  std::string drv_path;
  std::string debug_output;
  std::string debug_file;
  std::string source_path;
  std::string overlay_path;
  std::string source_hint;
  std::vector<Warning> warnings;
};

class NixResolver {
public:
  NixResolver(const std::string& nix_bin, const std::string& cache_root, bool fetch, bool offline,
              bool build_missing, bool refresh, bool no_cache);
  ~NixResolver() = default;

  DebugResolution resolve(const std::string& module_path, const std::string& build_id);
  const std::vector<std::string>& substituters();

private:
  std::string nix_bin_;
  std::string nix_store_bin_;
  std::string cache_root_;
  bool fetch_;
  bool offline_;
  bool build_missing_;
  bool refresh_;
  bool no_cache_;
  std::vector<std::string> substituters_;
  bool substituters_loaded_ = false;

  std::string cache_key(const std::string& module_path, const std::string& build_id) const;
  bool read_cache(const std::string& key, DebugResolution* out);
  void write_cache(const std::string& key, const DebugResolution& out);
  std::string run_nix(const std::vector<std::string>& args, bool* ok);
  const std::vector<std::string>& load_substituters();
  bool fetch_path(const std::string& path, std::vector<Warning>* warnings);
  std::string debug_file_for(const std::string& debug_output, const std::string& build_id,
                             const std::string& module_path);
  std::string readlink_path(const std::string& path);
  bool file_exists(const std::string& path);
};

} // namespace nix_sym
