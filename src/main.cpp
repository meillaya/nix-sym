#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "cli.hpp"
#include "core_reader.hpp"
#include "debuginfod.hpp"
#include "dwfl_session.hpp"
#include "gdb.hpp"
#include "json.hpp"
#include "nix.hpp"
#include "source.hpp"
#include "stack.hpp"

namespace nix_sym {
namespace {

std::string cache_root(const Options& options) {
  namespace fs = std::filesystem;
  if (!options.cache_dir.empty()) {
    return options.cache_dir;
  }
  std::string base;
  const char* xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg != nullptr && xdg[0] != '\0') {
    base = xdg;
  } else {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      base = std::string(home) + "/.cache";
    } else {
      base = fs::temp_directory_path().string();
    }
  }
  return base + "/nix-sym";
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

std::string persistent_core(const std::string& cache, const CoreFile& core) {
  std::string name = "core-" + fnv_hex(core.temp_path.empty() ? core.path : core.temp_path);
  std::string directory = cache + "/cores";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  std::string destination = directory + "/" + name + ".core";
  std::filesystem::path source = core.temp_path.empty() ? std::filesystem::path(core.path)
                                                        : std::filesystem::path(core.temp_path);
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    return source.string();
  }
  return destination;
}

void print_modules_text(const Backtrace& backtrace) {
  for (const auto& module : backtrace.modules) {
    std::cout << module_status_name(module.status) << "\t" << module.path;
    if (!module.build_id.empty()) {
      std::cout << "\tbuild-id " << module.build_id;
    }
    if (module.debuginfod) {
      std::cout << "\tdebuginfod";
    }
    if (!module.debug_output.empty()) {
      std::cout << "\tdebug " << module.debug_output;
    }
    if (!module.source_path.empty()) {
      std::cout << "\tsource " << module.source_path;
    }
    std::cout << "\n";
  }
}

int run(int argc, char** argv) {
  Options options = parse_cli(argc, argv);
  if (options.help) {
    print_usage(argv[0]);
    return 0;
  }
  if (options.version) {
    print_version();
    return 0;
  }

  std::string cache = cache_root(options);
  std::error_code ec;
  cache = std::filesystem::absolute(cache, ec).lexically_normal().string();
  if (ec) {
    cache = cache_root(options);
  }
  std::filesystem::create_directories(cache, ec);

  NixResolver resolver(options.nix_bin, cache, options.fetch, options.offline,
                       options.build_missing, options.refresh, options.no_cache);
  bool debuginfod_disabled = options.offline || options.no_debuginfod;
  DebuginfodClient debuginfod(cache, options.debuginfod_urls, debuginfod_disabled);
  bool materialize = options.extract_sources || (options.gdb_init && !options.no_extract);
  SourceResolver sources(cache, materialize, options.verbose > 1, options.source_dirs, &debuginfod);

  Backtrace backtrace;
  bool live = false;
  std::string gdb_core;
  if (!options.pid.empty()) {
    live = true;
    backtrace = symbolize_pid(options, std::stoi(options.pid), resolver, sources, &debuginfod);
  } else if (!options.stack.empty()) {
    std::vector<StackRecord> records = read_stack_file(options.stack);
    if (records.empty()) {
      throw std::runtime_error("stack file contains no addresses: " + options.stack);
    }
    backtrace = symbolize_stack(options, records, resolver, sources, &debuginfod);
  } else {
    CoreFile core = open_core_file(options.core, cache, options.coredumpctl_pid);
    CoreInfo core_info = read_core_info(core);
    backtrace = symbolize_core(options, core, resolver, sources, &debuginfod);
    backtrace.core.path = core_info.path;
    backtrace.core.compressed = core_info.compressed;
    if (backtrace.core.pid <= 0) {
      backtrace.core.pid = core_info.pid;
    }
    if (!core_info.exe.empty()) {
      backtrace.core.exe = core_info.exe;
    }
    if (core_info.signal != 0) {
      backtrace.core.signal = core_info.signal;
      backtrace.core.signal_name = core_info.signal_name;
    }
    if (options.gdb_init) {
      gdb_core = core.temp_path.empty() ? core.path : persistent_core(cache, core);
    }
  }

  if (options.list_modules) {
    backtrace.threads.clear();
  }

  bool gdb_to_stdout = options.gdb_init && options.gdb_init_path == "-";
  if (!gdb_to_stdout) {
    if (options.json) {
      std::cout << render_json(backtrace);
    } else if (options.list_modules) {
      print_modules_text(backtrace);
    } else {
      bool color = false;
      if (options.color == "always") {
        color = true;
      } else if (options.color == "auto") {
        color = isatty(STDOUT_FILENO) != 0 && std::getenv("NO_COLOR") == nullptr;
      }
      std::cout << render_text(backtrace, color, !options.no_demangle);
    }
  }

  if (options.gdb_init) {
    std::string content = render_gdb_init(backtrace, gdb_core, sources.mappings(), live);
    if (options.gdb_init_path == "-") {
      std::cout << content;
    } else {
      std::ofstream output(options.gdb_init_path, std::ios::binary | std::ios::trunc);
      if (!output) {
        std::cerr << "nix-sym: cannot write gdb init file: " << options.gdb_init_path << "\n";
        return 1;
      }
      output << content;
      if (!options.quiet) {
        std::cerr << "nix-sym: wrote " << options.gdb_init_path << "\n"
                  << "nix-sym: run `gdb -x " << options.gdb_init_path << "`\n";
      }
    }
  }

  if (options.strict && backtrace.partial) {
    return 2;
  }
  return backtrace.exit_code;
}

} // namespace
} // namespace nix_sym

int main(int argc, char** argv) {
  try {
    return nix_sym::run(argc, argv);
  } catch (const nix_sym::UsageError& error) {
    std::cerr << "nix-sym: " << error.what() << "\n\n";
    nix_sym::print_usage(argv[0]);
    return 64;
  } catch (const std::exception& error) {
    std::cerr << "nix-sym: " << error.what() << "\n";
    return 1;
  }
}
