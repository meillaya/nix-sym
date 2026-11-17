#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace nix_sym {

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct Options {
  std::string core;
  std::string coredumpctl_pid;
  std::string pid;
  std::string stack;
  bool json = false;
  bool bt = false;
  bool gdb_init = false;
  std::string gdb_init_path;
  bool list_modules = false;
  bool no_inline = false;
  bool no_demangle = false;
  bool strict = false;
  bool fetch = true;
  bool offline = false;
  bool build_missing = false;
  bool extract_sources = false;
  bool no_extract = false;
  std::string cache_dir;
  bool refresh = false;
  bool no_cache = false;
  std::vector<std::string> debug_dirs;
  std::vector<std::string> debuginfod_urls;
  bool no_debuginfod = false;
  std::vector<std::string> source_dirs;
  std::vector<int> threads;
  int max_frames = 256;
  std::string filter;
  std::string nix_bin = "nix";
  std::string cwd;
  std::string color = "auto";
  int verbose = 0;
  bool quiet = false;
  bool help = false;
  bool version = false;
};

Options parse_cli(int argc, char** argv);
void print_usage(const char* argv0);
void print_version();

} // namespace nix_sym
