#include "cli.hpp"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace nix_sym {
namespace {

std::string require_value(int& index, int argc, char** argv, const std::string& option) {
  if (index + 1 >= argc) {
    throw UsageError("missing value for " + option);
  }
  return argv[++index];
}

int parse_int(const std::string& value, const std::string& option) {
  try {
    size_t consumed = 0;
    int parsed = std::stoi(value, &consumed, 10);
    if (consumed != value.size()) {
      throw std::invalid_argument("trailing");
    }
    return parsed;
  } catch (const std::exception&) {
    throw UsageError("invalid integer for " + option + ": " + value);
  }
}

} // namespace

Options parse_cli(int argc, char** argv) {
  Options options;
  std::error_code ec;
  options.cwd = std::filesystem::current_path(ec).string();
  if (ec) {
    options.cwd = ".";
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::string value;
    std::string name = arg;
    auto equals = arg.find('=');
    if (equals != std::string::npos && arg.rfind("--", 0) == 0) {
      name = arg.substr(0, equals);
      value = arg.substr(equals + 1);
    }

    auto take = [&](const std::string& option) {
      if (!value.empty()) {
        return value;
      }
      return require_value(i, argc, argv, option);
    };

    if (name == "--help" || name == "-h") {
      options.help = true;
    } else if (name == "--version" || name == "-V") {
      options.version = true;
    } else if (name == "--core") {
      options.core = take(name);
    } else if (name == "--from-coredumpctl") {
      options.coredumpctl_pid = take(name);
    } else if (name == "--pid") {
      options.pid = take(name);
    } else if (name == "--stack") {
      options.stack = take(name);
    } else if (name == "--json") {
      options.json = true;
    } else if (name == "--bt") {
      options.bt = true;
    } else if (name == "--gdb-init") {
      options.gdb_init = true;
      options.gdb_init_path = take(name);
    } else if (name == "--list-modules") {
      options.list_modules = true;
    } else if (name == "--no-inline") {
      options.no_inline = true;
    } else if (name == "--no-demangle") {
      options.no_demangle = true;
    } else if (name == "--strict") {
      options.strict = true;
    } else if (name == "--fetch") {
      options.fetch = true;
    } else if (name == "--no-fetch") {
      options.fetch = false;
    } else if (name == "--offline") {
      options.offline = true;
      options.fetch = false;
    } else if (name == "--build-missing") {
      options.build_missing = true;
    } else if (name == "--extract-sources") {
      options.extract_sources = true;
    } else if (name == "--no-extract") {
      options.no_extract = true;
    } else if (name == "--cache-dir") {
      options.cache_dir = take(name);
    } else if (name == "--refresh") {
      options.refresh = true;
    } else if (name == "--no-cache") {
      options.no_cache = true;
    } else if (name == "--debug-dir") {
      options.debug_dirs.push_back(take(name));
    } else if (name == "--debuginfod") {
      options.debuginfod_urls.push_back(take(name));
    } else if (name == "--no-debuginfod") {
      options.no_debuginfod = true;
    } else if (name == "--source-dir") {
      options.source_dirs.push_back(take(name));
    } else if (name == "--thread") {
      options.threads.push_back(parse_int(take(name), name));
    } else if (name == "--max-frames") {
      options.max_frames = parse_int(take(name), name);
      if (options.max_frames < 0) {
        throw UsageError("--max-frames must not be negative");
      }
    } else if (name == "--filter") {
      options.filter = take(name);
    } else if (name == "--nix-bin") {
      options.nix_bin = take(name);
    } else if (name == "--cwd") {
      options.cwd = take(name);
    } else if (name == "--color") {
      options.color = take(name);
      if (options.color != "auto" && options.color != "always" && options.color != "never") {
        throw UsageError("--color must be auto, always, or never");
      }
    } else if (name == "--verbose" || name == "-v") {
      ++options.verbose;
    } else if (name == "--quiet" || name == "-q") {
      options.quiet = true;
    } else if (name == "--") {
      if (i + 1 < argc) {
        throw UsageError("unexpected positional argument: " + std::string(argv[i + 1]));
      }
    } else if (!arg.empty() && arg[0] == '-') {
      throw UsageError("unknown option: " + arg);
    } else {
      throw UsageError("unexpected positional argument: " + arg);
    }
  }

  if (options.help || options.version) {
    return options;
  }
  int input_count = 0;
  input_count += options.core.empty() ? 0 : 1;
  input_count += options.coredumpctl_pid.empty() ? 0 : 1;
  input_count += options.pid.empty() ? 0 : 1;
  input_count += options.stack.empty() ? 0 : 1;
  if (input_count != 1) {
    throw UsageError("exactly one of --core, --from-coredumpctl, --pid, or --stack is required");
  }
  if (!options.pid.empty()) {
    try {
      size_t consumed = 0;
      long parsed = std::stol(options.pid, &consumed, 10);
      if (consumed != options.pid.size() || parsed <= 0) {
        throw std::invalid_argument("bad pid");
      }
      options.pid = std::to_string(parsed);
    } catch (const std::exception&) {
      throw UsageError("invalid --pid: " + options.pid);
    }
  }
  if (!options.stack.empty() && options.gdb_init) {
    throw UsageError("--gdb-init is not supported with --stack");
  }
  for (const auto& mapping : options.source_dirs) {
    if (mapping.find('=') == std::string::npos || mapping.front() == '=' || mapping.back() == '=') {
      throw UsageError("--source-dir requires PREFIX=DIR");
    }
  }
  if (options.gdb_init && options.gdb_init_path.empty()) {
    throw UsageError("--gdb-init requires a path or -");
  }
  if (options.gdb_init && options.gdb_init_path == "-" && options.json) {
    throw UsageError("--gdb-init - cannot be combined with --json");
  }
  if (options.extract_sources && options.no_extract) {
    throw UsageError("--extract-sources conflicts with --no-extract");
  }
  return options;
}

void print_usage(const char* argv0) {
  std::cout << "Usage: " << argv0 << " --core PATH [options]\n"
            << "       " << argv0 << " --from-coredumpctl PID [options]\n"
            << "\n"
            << "Symbolize a Linux core dump from a Nix-built process.\n"
            << "\n"
            << "Input and output:\n"
            << "  --core PATH             Core file, .zst core, or - for stdin\n"
            << "  --from-coredumpctl PID  Read a core through coredumpctl dump\n"
            << "  --bt                    Print the backtrace (default)\n"
            << "  --json                  Emit schema-versioned JSON on stdout\n"
            << "  --gdb-init PATH         Emit a gdb init file (- for stdout)\n"
            << "  --list-modules          Print module resolution only\n"
            << "  --thread TID            Select a thread (repeatable)\n"
            << "  --max-frames N          Frame cap per thread (default 256, 0 unlimited)\n"
            << "  --filter REGEX          Keep frames whose demangled name matches\n"
            << "  --no-inline             Disable inline frames\n"
            << "  --no-demangle           Keep raw symbol names\n"
            << "  --strict                Exit 2 when any frame or module is unresolved\n"
            << "\n"
            << "Nix resolution:\n"
            << "  --fetch                 Fetch missing debug outputs (default)\n"
            << "  --no-fetch, --offline   Do not use the network\n"
            << "  --build-missing         Allow nix-store -r to build a missing debug output\n"
            << "  --debug-dir DIR         Extra debug file directory (repeatable)\n"
            << "  --nix-bin PATH          Nix executable (default nix)\n"
            << "  --cache-dir DIR         Cache directory\n"
            << "  --refresh               Ignore cached resolutions\n"
            << "  --no-cache              Disable cache reads and writes\n"
            << "  --extract-sources       Extract source archives and overlays\n"
            << "  --no-extract            Do not extract sources for --gdb-init\n"
            << "\n"
            << "Other:\n"
            << "  --cwd DIR               Working directory for relative module paths\n"
            << "  --color auto|always|never\n"
            << "  -v, --verbose           More progress on stderr\n"
            << "  -q, --quiet             Suppress progress\n"
            << "  -h, --help              Show this help\n"
            << "  -V, --version           Show the version\n";
}

void print_version() {
  std::cout << "nix-sym 0.3.0\n";
}

} // namespace nix_sym
