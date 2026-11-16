#include "dwfl_session.hpp"

#include <dwarf.h>
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <fcntl.h>
#include <gelf.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cli.hpp"
#include "demangle.hpp"

namespace nix_sym {
namespace {

constexpr int kInlineLimit = 32;

std::string hex_bytes(const unsigned char* bytes, size_t size) {
  static const char* digits = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    result.push_back(digits[bytes[i] >> 4]);
    result.push_back(digits[bytes[i] & 0x0f]);
  }
  return result;
}

std::string module_build_id(Dwfl_Module* module) {
  const unsigned char* bits = nullptr;
  GElf_Addr vaddr = 0;
  int size = dwfl_module_build_id(module, &bits, &vaddr);
  if (size <= 0 || bits == nullptr) {
    return {};
  }
  return hex_bytes(bits, static_cast<size_t>(size));
}

std::string module_main_file(Dwfl_Module* module) {
  const char* mainfile = nullptr;
  const char* debugfile = nullptr;
  dwfl_module_info(module, nullptr, nullptr, nullptr, nullptr, nullptr, &mainfile, &debugfile);
  if (mainfile != nullptr) {
    return mainfile;
  }
  return {};
}

std::string die_linkage_name(Dwarf_Die* die) {
  if (die == nullptr) {
    return {};
  }
  Dwarf_Attribute attribute;
  for (unsigned name : {static_cast<unsigned>(DW_AT_linkage_name),
                        static_cast<unsigned>(DW_AT_MIPS_linkage_name)}) {
    Dwarf_Attribute* found = dwarf_attr_integrate(die, name, &attribute);
    if (found != nullptr) {
      const char* value = dwarf_formstring(found);
      if (value != nullptr && value[0] != '\0') {
        return value;
      }
    }
  }
  return {};
}

std::string die_name(Dwarf_Die* die, int depth = 0) {
  if (die == nullptr || depth > 16) {
    return {};
  }
  Dwarf_Attribute attribute;
  Dwarf_Attribute* origin = dwarf_attr_integrate(die, DW_AT_abstract_origin, &attribute);
  if (origin != nullptr) {
    Dwarf_Die origin_die;
    if (dwarf_formref_die(origin, &origin_die) != nullptr) {
      std::string origin_name = die_name(&origin_die, depth + 1);
      if (!origin_name.empty()) {
        return origin_name;
      }
    }
  }
  Dwarf_Attribute* name = dwarf_attr_integrate(die, DW_AT_name, &attribute);
  if (name != nullptr) {
    const char* value = dwarf_formstring(name);
    if (value != nullptr) {
      return value;
    }
  }
  return {};
}

bool unsigned_attribute(Dwarf_Die* die, unsigned attribute_name, Dwarf_Word* value) {
  Dwarf_Attribute attribute;
  Dwarf_Attribute* found = dwarf_attr(die, attribute_name, &attribute);
  if (found == nullptr) {
    return false;
  }
  return dwarf_formudata(found, value) == 0;
}

std::string call_file_name(Dwarf_Die* cudie, Dwarf_Die* inline_die) {
  Dwarf_Word file_index = 0;
  if (!unsigned_attribute(inline_die, DW_AT_call_file, &file_index) || file_index == 0) {
    return {};
  }
  Dwarf_Files* files = nullptr;
  size_t file_count = 0;
  if (dwarf_getsrcfiles(cudie, &files, &file_count) != 0 || files == nullptr) {
    return {};
  }
  const char* name = dwarf_filesrc(files, static_cast<size_t>(file_index), nullptr, nullptr);
  return name == nullptr ? std::string() : std::string(name);
}

std::string resolve_relative_path(const std::string& path, const std::string& cwd) {
  if (path.empty() || path[0] == '/') {
    return path;
  }
  std::error_code ec;
  std::filesystem::path candidate = std::filesystem::path(cwd) / path;
  if (std::filesystem::exists(candidate, ec)) {
    return candidate.lexically_normal().string();
  }
  return path;
}

struct Session {
  const Options* options = nullptr;
  NixResolver* resolver = nullptr;
  SourceResolver* sources = nullptr;
  Backtrace backtrace;
  Dwfl* dwfl = nullptr;
  char* debuginfo_path = nullptr;
  std::unordered_map<Dwfl_Module*, size_t> module_index;
  std::regex filter;
  bool has_filter = false;
  bool filter_valid = true;

  ~Session() {
    if (dwfl != nullptr) {
      dwfl_end(dwfl);
      dwfl = nullptr;
    }
    std::free(debuginfo_path);
    debuginfo_path = nullptr;
  }
};

struct FrameCallbackState {
  Session* session = nullptr;
  ThreadInfo* thread = nullptr;
};

ModuleInfo* module_for(Session& session, Dwfl_Module* module) {
  auto found = session.module_index.find(module);
  if (found == session.module_index.end()) {
    return nullptr;
  }
  return &session.backtrace.modules[found->second];
}

std::string language_for_cu(Dwarf_Die* cudie) {
  if (cudie == nullptr) {
    return {};
  }
  Dwarf_Attribute attribute;
  Dwarf_Attribute* found = dwarf_attr(cudie, DW_AT_language, &attribute);
  if (found == nullptr) {
    return {};
  }
  Dwarf_Word value = 0;
  if (dwarf_formudata(found, &value) != 0) {
    return {};
  }
  return language_name(language_from_dwarf(static_cast<unsigned>(value)));
}

int frame_callback(Dwfl_Frame* state, void* argument) {
  auto* callback = static_cast<FrameCallbackState*>(argument);
  Session& session = *callback->session;
  ThreadInfo& thread = *callback->thread;

  Dwarf_Addr pc = 0;
  bool activation = false;
  if (!dwfl_frame_pc(state, &pc, &activation)) {
    return DWARF_CB_OK;
  }
  if (session.options->max_frames > 0 &&
      static_cast<int>(thread.frames.size()) >= session.options->max_frames) {
    return DWARF_CB_ABORT;
  }

  Dwarf_Addr lookup_pc = activation ? pc : (pc > 0 ? pc - 1 : pc);
  Dwfl_Module* module = dwfl_addrmodule(session.dwfl, lookup_pc);
  ModuleInfo* module_info = module != nullptr ? module_for(session, module) : nullptr;

  Frame frame;
  frame.pc = pc;
  frame.lookup_pc = lookup_pc;
  frame.activation = activation;
  if (module_info != nullptr) {
    frame.module_path = module_info->path;
    frame.module_name = module_info->name;
    frame.build_id = module_info->build_id;
  }

  Language language = Language::Unknown;
  Dwarf_Die* cudie = nullptr;
  Dwarf_Addr cubias = 0;
  if (module != nullptr) {
    GElf_Sym symbol;
    GElf_Word symbol_section = 0;
    const char* name = dwfl_module_addrsym(module, lookup_pc, &symbol, &symbol_section);
    if (name != nullptr && name[0] != '\0') {
      bool contains = symbol.st_size == 0 || (lookup_pc >= symbol.st_value &&
                                              lookup_pc < symbol.st_value + symbol.st_size);
      if (contains) {
        frame.mangled = name;
        frame.has_symbol = true;
      }
    }
    if (!frame.has_symbol) {
      const char* fallback = dwfl_module_addrname(module, lookup_pc);
      if (fallback != nullptr && fallback[0] != '\0') {
        frame.mangled = fallback;
        frame.has_symbol = true;
      }
    }

    cudie = dwfl_module_addrdie(module, lookup_pc, &cubias);
    if (!frame.has_symbol && cudie != nullptr) {
      Dwarf_Die* scopes = nullptr;
      int scope_count = dwarf_getscopes(cudie, lookup_pc - cubias, &scopes);
      if (scope_count > 0 && scopes != nullptr) {
        for (int i = 0; i < scope_count; ++i) {
          if (dwarf_tag(&scopes[i]) != DW_TAG_subprogram) {
            continue;
          }
          std::string linkage = die_linkage_name(&scopes[i]);
          std::string name = linkage.empty() ? die_name(&scopes[i]) : linkage;
          if (!name.empty()) {
            frame.mangled = name;
            frame.has_symbol = true;
            break;
          }
        }
        std::free(scopes);
      }
    }
    std::string cu_language = language_for_cu(cudie);
    if (!cu_language.empty()) {
      if (cu_language == "c++") {
        language = Language::Cxx;
      } else if (cu_language == "rust") {
        language = Language::Rust;
      } else if (cu_language == "haskell") {
        language = Language::Haskell;
      } else if (cu_language == "c") {
        language = Language::C;
      }
    }
    if (language == Language::Unknown && frame.has_symbol) {
      language = language_from_symbol(frame.mangled);
    }
    frame.language = language_name(language);
    if (frame.has_symbol && !session.options->no_demangle) {
      frame.demangled = demangle_symbol(frame.mangled, language);
    }

    Dwfl_Line* line = dwfl_module_getsrc(module, lookup_pc);
    if (line != nullptr && module_info != nullptr) {
      Dwarf_Addr line_address = 0;
      int line_number = 0;
      int column = 0;
      const char* file =
          dwfl_lineinfo(line, &line_address, &line_number, &column, nullptr, nullptr);
      const char* comp_dir = dwfl_line_comp_dir(line);
      if (file != nullptr && file[0] != '\0') {
        frame.location =
            session.sources->map(*module_info, comp_dir != nullptr ? comp_dir : "", file);
        frame.location.line = line_number;
        frame.location.column = column;
        frame.has_line = true;
      }
    }

    if (!session.options->no_inline && cudie != nullptr) {
      Dwarf_Die* scopes = nullptr;
      int scope_count = dwarf_getscopes(cudie, lookup_pc - cubias, &scopes);
      if (scope_count > 0 && scopes != nullptr) {
        std::string previous;
        for (int i = 0; i < scope_count; ++i) {
          if (dwarf_tag(&scopes[i]) != DW_TAG_inlined_subroutine) {
            continue;
          }
          std::string inline_name = die_name(&scopes[i]);
          if (inline_name.empty() || inline_name == previous) {
            continue;
          }
          InlineFrame inline_frame;
          inline_frame.mangled = inline_name;
          inline_frame.language = language_name(language);
          if (!session.options->no_demangle) {
            inline_frame.demangled = demangle_symbol(inline_name, language);
          }
          Dwarf_Word call_line = 0;
          if (unsigned_attribute(&scopes[i], DW_AT_call_line, &call_line)) {
            std::string file = call_file_name(cudie, &scopes[i]);
            if (!file.empty() && module_info != nullptr) {
              const char* comp_dir = nullptr;
              if (line != nullptr) {
                comp_dir = dwfl_line_comp_dir(line);
              }
              inline_frame.call =
                  session.sources->map(*module_info, comp_dir != nullptr ? comp_dir : "", file);
              inline_frame.call.line = static_cast<int>(call_line);
              inline_frame.call.valid = true;
            }
          }
          frame.inline_frames.push_back(std::move(inline_frame));
          previous = frame.inline_frames.back().mangled;
          if (frame.inline_frames.size() >= static_cast<size_t>(kInlineLimit)) {
            break;
          }
        }
        std::free(scopes);
      }
    }
  }

  if (!frame.has_symbol && !frame.inline_frames.empty()) {
    frame.mangled = frame.inline_frames.front().mangled;
    frame.demangled = frame.inline_frames.front().demangled;
    frame.has_symbol = true;
  }
  if (session.has_filter) {
    const std::string& name = frame.demangled.empty() ? frame.mangled : frame.demangled;
    if (!std::regex_search(name, session.filter)) {
      return DWARF_CB_OK;
    }
  }
  frame.index = static_cast<unsigned>(thread.frames.size());
  thread.frames.push_back(std::move(frame));
  return DWARF_CB_OK;
}

int thread_callback(Dwfl_Thread* thread, void* argument) {
  auto* session = static_cast<Session*>(argument);
  int tid = dwfl_thread_tid(thread);
  if (!session->options->threads.empty() &&
      std::find(session->options->threads.begin(), session->options->threads.end(), tid) ==
          session->options->threads.end()) {
    return DWARF_CB_OK;
  }
  ThreadInfo info;
  info.tid = tid;
  FrameCallbackState state{session, &info};
  int result = dwfl_thread_getframes(thread, frame_callback, &state);
  if (result < 0 && info.frames.empty()) {
    session->backtrace.warnings.push_back(
        {"unwind-failed", {}, "could not unwind thread " + std::to_string(tid)});
  }
  session->backtrace.threads.push_back(std::move(info));
  return DWARF_CB_OK;
}

int module_callback(Dwfl_Module* module, void**, const char* module_name, Dwarf_Addr base,
                    void* argument) {
  auto* session = static_cast<Session*>(argument);
  (void)base;
  GElf_Addr bias = 0;
  dwfl_module_getelf(module, &bias);
  std::string path = module_main_file(module);
  std::string name = module_name != nullptr ? module_name : "";
  if (path.empty()) {
    path = name;
  }
  path = resolve_relative_path(path, session->options->cwd);
  std::string build_id = module_build_id(module);
  DebugResolution resolution = session->resolver->resolve(path, build_id);

  ModuleInfo info;
  info.name = name;
  info.path = path;
  info.build_id = build_id;
  info.drv_path = resolution.drv_path;
  info.debug_output = resolution.debug_output;
  info.debug_file = resolution.debug_file;
  info.source_path = resolution.source_path;
  info.overlay_path = resolution.overlay_path;
  info.status = resolution.status;
  info.resolved = resolution.status == ModuleStatus::Resolved;
  if (!info.source_path.empty()) {
    std::error_code ec;
    info.source_kind =
        std::filesystem::is_directory(info.source_path, ec) ? "directory" : "archive";
    info.source_status =
        std::filesystem::exists(info.source_path, ec) ? "available" : "unavailable";
  }
  if (info.source_kind != "none" && !info.source_path.empty()) {
    info.source_root = std::filesystem::path(info.source_path).filename().string();
  }

  session->backtrace.warnings.insert(session->backtrace.warnings.end(), resolution.warnings.begin(),
                                     resolution.warnings.end());
  session->module_index[module] = session->backtrace.modules.size();
  session->backtrace.modules.push_back(std::move(info));
  return DWARF_CB_OK;
}

} // namespace

Backtrace symbolize_core(const Options& options, CoreFile& core, NixResolver& resolver,
                         SourceResolver& sources) {
  Session session;
  session.options = &options;
  session.resolver = &resolver;
  session.sources = &sources;
  session.backtrace.core.path = core.path;
  session.backtrace.core.compressed = core.compressed;
  session.backtrace.exit_code = 3;

  if (!options.filter.empty()) {
    try {
      session.filter = std::regex(options.filter, std::regex::ECMAScript);
      session.has_filter = true;
    } catch (const std::regex_error&) {
      session.filter_valid = false;
      session.backtrace.warnings.push_back(
          {"invalid-filter", {}, "ignoring invalid --filter expression: " + options.filter});
    }
  }

  Dwfl_Callbacks callbacks{};
  callbacks.find_elf = dwfl_build_id_find_elf;
  callbacks.find_debuginfo = dwfl_standard_find_debuginfo;
  callbacks.section_address = nullptr;
  callbacks.debuginfo_path = &session.debuginfo_path;

  session.dwfl = dwfl_begin(&callbacks);
  if (session.dwfl == nullptr) {
    session.backtrace.warnings.push_back(
        {"dwfl-error", {}, std::string("cannot initialise libdwfl: ") + dwfl_errmsg(-1)});
    return session.backtrace;
  }

  int reported = dwfl_core_file_report(session.dwfl, core.elf, nullptr);
  if (reported < 0) {
    session.backtrace.warnings.push_back(
        {"core-unreadable", {}, std::string("cannot read core modules: ") + dwfl_errmsg(-1)});
    return session.backtrace;
  }

  dwfl_getmodules(session.dwfl, module_callback, &session, 0);

  std::vector<std::string> debug_dirs = options.debug_dirs;
  for (const auto& module : session.backtrace.modules) {
    if (!module.debug_output.empty()) {
      debug_dirs.push_back(module.debug_output + "/lib/debug");
    }
  }
  std::sort(debug_dirs.begin(), debug_dirs.end());
  debug_dirs.erase(std::unique(debug_dirs.begin(), debug_dirs.end()), debug_dirs.end());
  std::string joined;
  for (const auto& directory : debug_dirs) {
    if (!joined.empty()) {
      joined.push_back(':');
    }
    joined += directory;
  }
  session.debuginfo_path = strdup(joined.c_str());

  int pid = dwfl_core_file_attach(session.dwfl, core.elf);
  if (pid < 0) {
    session.backtrace.warnings.push_back(
        {"core-unreadable", {}, std::string("cannot attach core state: ") + dwfl_errmsg(-1)});
    return session.backtrace;
  }
  if (session.backtrace.core.pid <= 0) {
    session.backtrace.core.pid = pid;
  }

  int threads = dwfl_getthreads(session.dwfl, thread_callback, &session);
  if (threads < 0) {
    session.backtrace.warnings.push_back(
        {"unwind-failed", {}, std::string("cannot enumerate threads: ") + dwfl_errmsg(-1)});
  }

  std::sort(session.backtrace.modules.begin(), session.backtrace.modules.end(),
            [](const ModuleInfo& a, const ModuleInfo& b) { return a.path < b.path; });
  std::sort(session.backtrace.threads.begin(), session.backtrace.threads.end(),
            [](const ThreadInfo& a, const ThreadInfo& b) { return a.tid < b.tid; });
  std::sort(session.backtrace.warnings.begin(), session.backtrace.warnings.end(),
            [](const Warning& a, const Warning& b) {
              if (a.code != b.code) {
                return a.code < b.code;
              }
              if (a.module != b.module) {
                return a.module < b.module;
              }
              return a.message < b.message;
            });
  session.backtrace.warnings.erase(
      std::unique(session.backtrace.warnings.begin(), session.backtrace.warnings.end(),
                  [](const Warning& a, const Warning& b) {
                    return a.code == b.code && a.module == b.module && a.message == b.message;
                  }),
      session.backtrace.warnings.end());

  bool has_frames = false;
  bool incomplete = !session.backtrace.warnings.empty();
  for (const auto& thread : session.backtrace.threads) {
    for (const auto& frame : thread.frames) {
      has_frames = true;
      if (!frame.has_symbol) {
        incomplete = true;
      }
    }
  }
  session.backtrace.partial = incomplete;
  session.backtrace.exit_code = has_frames ? 0 : 3;
  return session.backtrace;
}

} // namespace nix_sym
