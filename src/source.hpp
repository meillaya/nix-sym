#pragma once

#include <map>
#include <string>
#include <vector>

#include "model.hpp"

namespace nix_sym {

class DebuginfodClient;

struct SourceMapping {
  std::string module;
  std::string comp_dir;
  std::string root;
  std::string source_path;
  std::string overlay_path;
  std::string kind = "none";
  std::string resolved_root;
  std::string status = "unresolved";
};

class SourceResolver {
public:
  SourceResolver(const std::string& cache_root, bool materialize, bool verbose,
                 const std::vector<std::string>& source_dirs = {},
                 DebuginfodClient* debuginfod = nullptr);
  ~SourceResolver() = default;

  SourceLocation map(const ModuleInfo& module, const std::string& comp_dir, const std::string& file,
                     bool force_materialize = false);
  std::vector<SourceMapping> mappings() const;
  const std::string& cache_root() const {
    return cache_root_;
  }

private:
  std::string cache_root_;
  bool materialize_;
  bool verbose_;
  std::vector<std::pair<std::string, std::string>> source_maps_;
  DebuginfodClient* debuginfod_ = nullptr;
  std::map<std::string, SourceMapping> mappings_;

  SourceMapping module_mapping(const ModuleInfo& module);
  std::string hash_text(const std::string& text) const;
  bool ensure_source(const SourceMapping& mapping, std::string* resolved_root, std::string* status);
  bool extract_archive(const std::string& archive, const std::string& destination,
                       const std::string& root_hint, std::string* extracted_root,
                       std::string* status);
  bool merge_overlay(const std::string& root, const std::string& overlay,
                     const std::string& root_name, std::string* status);
  bool copy_tree_with_overlay(const std::string& source, const std::string& overlay,
                              const std::string& root_hint, const std::string& destination,
                              std::string* status);
  std::string find_source_root(const std::string& base, const std::string& root_hint);
};

} // namespace nix_sym
