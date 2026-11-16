#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nix_sym {

enum class ModuleStatus {
  Resolved,
  MissingDebug,
  MissingFile,
  NoBuildId,
  NonNix,
  Unknown,
};

const char* module_status_name(ModuleStatus status);

struct SourceLocation {
  bool valid = false;
  std::string original;
  std::string comp_dir;
  int line = 0;
  int column = 0;
  std::string resolved;
  std::string status = "unresolved";
};

struct InlineFrame {
  std::string mangled;
  std::string demangled;
  std::string language;
  SourceLocation call;
};

struct Frame {
  unsigned index = 0;
  std::uint64_t pc = 0;
  std::uint64_t lookup_pc = 0;
  bool activation = false;
  std::string module_path;
  std::string module_name;
  std::string build_id;
  std::string mangled;
  std::string demangled;
  std::string language;
  bool has_symbol = false;
  bool has_line = false;
  SourceLocation location;
  std::vector<InlineFrame> inline_frames;
};

struct ModuleInfo {
  std::string name;
  std::string path;
  std::string build_id;
  std::string drv_path;
  std::string debug_output;
  std::string debug_file;
  std::string source_path;
  std::string overlay_path;
  std::string source_root;
  std::string source_kind = "none";
  std::string source_status = "unavailable";
  ModuleStatus status = ModuleStatus::Unknown;
  bool resolved = false;
};

struct ThreadInfo {
  int tid = 0;
  std::vector<Frame> frames;
};

struct CoreInfo {
  std::string path;
  bool compressed = false;
  int pid = -1;
  std::string exe;
  int signal = 0;
  std::string signal_name;
  bool truncated = false;
  bool elf = true;
};

struct Warning {
  std::string code;
  std::string module;
  std::string message;
};

struct Backtrace {
  CoreInfo core;
  std::vector<ModuleInfo> modules;
  std::vector<ThreadInfo> threads;
  std::vector<Warning> warnings;
  bool partial = false;
  int exit_code = 0;
};

} // namespace nix_sym
