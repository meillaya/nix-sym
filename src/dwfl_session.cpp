#include "dwfl_session.hpp"

#include <dwarf.h>
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <fcntl.h>
#include <gelf.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cli.hpp"
#include "debuginfod.hpp"
#include "demangle.hpp"
#include "stack.hpp"

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

std::string read_link(const std::string& path) {
  std::error_code ec;
  std::filesystem::path target = std::filesystem::read_symlink(path, ec);
  return ec ? std::string() : target.string();
}

std::string hex_value(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

std::string dwfl_error() {
  const char* message = dwfl_errmsg(-1);
  return message != nullptr ? std::string(message) : std::string("unknown libdwfl error");
}

// Read the thread name for a live process. /proc entries vanish when the
// process exits mid-unwind, so a missing file is not an error here. Control
// bytes are replaced so the name is safe to print in the text renderer.
std::string read_thread_name(int pid, int tid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/comm",
                      std::ios::binary);
  if (!input) {
    return {};
  }
  std::string name;
  std::getline(input, name);
  constexpr std::size_t kThreadNameLimit = 64;
  if (name.size() > kThreadNameLimit) {
    name.resize(kThreadNameLimit);
  }
  for (char& character : name) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7f) {
      character = '?';
    }
  }
  return name;
}

std::string debug_file_path(const std::string& debug_output, const std::string& build_id) {
  if (debug_output.empty() || build_id.size() < 2) {
    return {};
  }
  return debug_output + "/lib/debug/.build-id/" + build_id.substr(0, 2) + "/" + build_id.substr(2) +
         ".debug";
}

Language language_from_string(const std::string& name) {
  if (name == "c++") {
    return Language::Cxx;
  }
  if (name == "rust") {
    return Language::Rust;
  }
  if (name == "haskell") {
    return Language::Haskell;
  }
  if (name == "c") {
    return Language::C;
  }
  return Language::Unknown;
}

struct PendingModule {
  Dwfl_Module* module = nullptr;
  std::string name;
  std::string path;
  std::string build_id;
};

struct Session {
  const Options* options = nullptr;
  NixResolver* resolver = nullptr;
  SourceResolver* sources = nullptr;
  DebuginfodClient* debuginfod = nullptr;
  Backtrace backtrace;
  Dwfl* dwfl = nullptr;
  Dwfl_Callbacks callbacks{};
  char* debuginfo_path = nullptr;
  std::unordered_map<Dwfl_Module*, size_t> module_index;
  std::vector<PendingModule> pending;
  std::regex filter;
  bool has_filter = false;
  // Process id for live sessions; 0 for core and stack input. Used to read
  // thread names and to report process-exit races differently.
  int live_pid = 0;

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

void symbolize_address(Session& session, Dwfl_Module* module, Dwarf_Addr lookup,
                       ModuleInfo* module_info, Frame* frame) {
  if (module == nullptr) {
    return;
  }

  GElf_Sym symbol;
  GElf_Word symbol_section = 0;
  const char* name = dwfl_module_addrsym(module, lookup, &symbol, &symbol_section);
  if (name != nullptr && name[0] != '\0') {
    bool contains = symbol.st_size == 0 ||
                    (lookup >= symbol.st_value && lookup < symbol.st_value + symbol.st_size);
    if (contains) {
      frame->mangled = name;
      frame->has_symbol = true;
    }
  }
  if (!frame->has_symbol) {
    const char* fallback = dwfl_module_addrname(module, lookup);
    if (fallback != nullptr && fallback[0] != '\0') {
      frame->mangled = fallback;
      frame->has_symbol = true;
    }
  }

  Dwarf_Addr cubias = 0;
  Dwarf_Die* cudie = dwfl_module_addrdie(module, lookup, &cubias);
  Language language = Language::Unknown;
  if (cudie != nullptr) {
    language = language_from_string(language_for_cu(cudie));
  }

  Dwarf_Die* scopes = nullptr;
  int scope_count = 0;
  if (cudie != nullptr) {
    scope_count = dwarf_getscopes(cudie, lookup - cubias, &scopes);
  }
  if (!frame->has_symbol && scope_count > 0 && scopes != nullptr) {
    for (int i = 0; i < scope_count; ++i) {
      if (dwarf_tag(&scopes[i]) != DW_TAG_subprogram) {
        continue;
      }
      std::string linkage = die_linkage_name(&scopes[i]);
      std::string die_function = linkage.empty() ? die_name(&scopes[i]) : linkage;
      if (!die_function.empty()) {
        frame->mangled = die_function;
        frame->has_symbol = true;
        break;
      }
    }
  }
  if (language == Language::Unknown && frame->has_symbol) {
    language = language_from_symbol(frame->mangled);
  }
  frame->language = language_name(language);
  if (frame->has_symbol && !session.options->no_demangle) {
    frame->demangled = demangle_symbol(frame->mangled, language);
  }

  Dwfl_Line* line = dwfl_module_getsrc(module, lookup);
  if (line != nullptr && module_info != nullptr) {
    Dwarf_Addr line_address = 0;
    int line_number = 0;
    int column = 0;
    const char* file = dwfl_lineinfo(line, &line_address, &line_number, &column, nullptr, nullptr);
    const char* comp_dir = dwfl_line_comp_dir(line);
    if (file != nullptr && file[0] != '\0') {
      frame->location =
          session.sources->map(*module_info, comp_dir != nullptr ? comp_dir : "", file);
      frame->location.line = line_number;
      frame->location.column = column;
      frame->has_line = true;
    }
  }

  if (!session.options->no_inline && scope_count > 0 && scopes != nullptr) {
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
          const char* comp_dir = line != nullptr ? dwfl_line_comp_dir(line) : nullptr;
          inline_frame.call =
              session.sources->map(*module_info, comp_dir != nullptr ? comp_dir : "", file);
          inline_frame.call.line = static_cast<int>(call_line);
          inline_frame.call.valid = true;
        }
      }
      frame->inline_frames.push_back(std::move(inline_frame));
      previous = frame->inline_frames.back().mangled;
      if (frame->inline_frames.size() >= static_cast<size_t>(kInlineLimit)) {
        break;
      }
    }
  }
  if (!frame->has_symbol && !frame->inline_frames.empty()) {
    frame->mangled = frame->inline_frames.front().mangled;
    frame->demangled = frame->inline_frames.front().demangled;
    frame->has_symbol = true;
  }
  std::free(scopes);
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
  symbolize_address(session, module, lookup_pc, module_info, &frame);

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
  if (session->live_pid > 0) {
    info.name = read_thread_name(session->live_pid, tid);
  }
  FrameCallbackState state{session, &info};
  int result = dwfl_thread_getframes(thread, frame_callback, &state);
  if (result < 0) {
    if (session->live_pid > 0) {
      // The process can exit or the thread can be reaped while we unwind it.
      // Keep the frames collected so far and report the race clearly.
      session->backtrace.warnings.push_back(
          {"live-thread-gone",
           {},
           "thread " + std::to_string(tid) + " disappeared while unwinding: " + dwfl_error() +
               " (kept " + std::to_string(info.frames.size()) + " collected frame(s))"});
    } else if (info.frames.empty()) {
      session->backtrace.warnings.push_back(
          {"unwind-failed", {}, "could not unwind thread " + std::to_string(tid)});
    }
  }
  session->backtrace.threads.push_back(std::move(info));
  return DWARF_CB_OK;
}

int collect_module_callback(Dwfl_Module* module, void**, const char* module_name, Dwarf_Addr base,
                            void* argument) {
  auto* session = static_cast<Session*>(argument);
  (void)base;
  GElf_Addr bias = 0;
  dwfl_module_getelf(module, &bias);
  PendingModule pending;
  pending.module = module;
  pending.name = module_name != nullptr ? module_name : "";
  pending.path = module_main_file(module);
  if (pending.path.empty()) {
    pending.path = pending.name;
  }
  pending.path = resolve_relative_path(pending.path, session->options->cwd);
  pending.build_id = module_build_id(module);
  session->pending.push_back(std::move(pending));
  return DWARF_CB_OK;
}

void configure_debuginfo_path(Session& session) {
  std::vector<std::string> debug_dirs = session.options->debug_dirs;
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
  std::free(session.debuginfo_path);
  session.debuginfo_path = strdup(joined.c_str());
}

void resolve_pending(Session& session) {
  std::vector<ModuleRequest> requests;
  requests.reserve(session.pending.size());
  for (const auto& pending : session.pending) {
    requests.push_back({pending.path, pending.build_id});
  }
  std::vector<DebugResolution> resolutions = session.resolver->resolve_batch(requests);

  for (size_t i = 0; i < session.pending.size(); ++i) {
    PendingModule& pending = session.pending[i];
    DebugResolution resolution = std::move(resolutions[i]);

    if (resolution.status != ModuleStatus::Resolved && session.debuginfod != nullptr &&
        session.debuginfod->enabled() && !pending.build_id.empty()) {
      DebuginfodResult fetched = session.debuginfod->find_debuginfo(pending.build_id);
      if (fetched.ok) {
        std::string error;
        std::string debug_output =
            session.debuginfod->install_debuginfo(pending.build_id, fetched.path, &error);
        if (!debug_output.empty()) {
          resolution.status = ModuleStatus::Resolved;
          resolution.debug_output = debug_output;
          resolution.debug_file = debug_file_path(debug_output, pending.build_id);
          resolution.warnings.erase(std::remove_if(resolution.warnings.begin(),
                                                   resolution.warnings.end(),
                                                   [](const Warning& warning) {
                                                     return warning.code == "debug-info-missing";
                                                   }),
                                    resolution.warnings.end());
        } else {
          resolution.warnings.push_back({"debuginfod-failed", pending.path,
                                         "could not install fetched debug info: " + error});
        }
      } else {
        resolution.warnings.push_back(
            {"debuginfod-failed", pending.path, "debuginfod lookup failed: " + fetched.error});
      }
    }

    if (resolution.status == ModuleStatus::Resolved && resolution.source_path.empty() &&
        !resolution.source_hint.empty()) {
      resolution.source_path = resolution.source_hint;
    }

    ModuleInfo info;
    info.name = pending.name;
    info.path = pending.path;
    info.build_id = pending.build_id;
    info.drv_path = resolution.drv_path;
    info.debug_output = resolution.debug_output;
    info.debug_file = resolution.debug_file;
    info.source_path = resolution.source_path;
    info.overlay_path = resolution.overlay_path;
    info.status = resolution.status;
    info.resolved = resolution.status == ModuleStatus::Resolved;
    info.debuginfod = resolution.debug_output.find("/debuginfod/") != std::string::npos;
    if (!info.source_path.empty()) {
      std::error_code ec;
      info.source_kind =
          std::filesystem::is_directory(info.source_path, ec) ? "directory" : "archive";
      info.source_status =
          std::filesystem::exists(info.source_path, ec) ? "available" : "unavailable";
      info.source_root = std::filesystem::path(info.source_path).filename().string();
    }

    session.backtrace.warnings.insert(session.backtrace.warnings.end(), resolution.warnings.begin(),
                                      resolution.warnings.end());
    session.module_index[pending.module] = session.backtrace.modules.size();
    session.backtrace.modules.push_back(std::move(info));
  }
}

bool init_session(Session& session, bool offline, bool live) {
  session.callbacks.find_elf = live ? dwfl_linux_proc_find_elf : dwfl_build_id_find_elf;
  session.callbacks.find_debuginfo = dwfl_standard_find_debuginfo;
  session.callbacks.section_address = offline ? dwfl_offline_section_address : nullptr;
  session.callbacks.debuginfo_path = &session.debuginfo_path;

  if (!session.options->filter.empty()) {
    try {
      session.filter = std::regex(session.options->filter, std::regex::ECMAScript);
      session.has_filter = true;
    } catch (const std::regex_error&) {
      session.backtrace.warnings.push_back(
          {"invalid-filter",
           {},
           "ignoring invalid --filter expression: " + session.options->filter});
    }
  }

  session.dwfl = dwfl_begin(&session.callbacks);
  if (session.dwfl == nullptr) {
    session.backtrace.warnings.push_back(
        {"dwfl-error", {}, std::string("cannot initialise libdwfl: ") + dwfl_errmsg(-1)});
    return false;
  }
  return true;
}

void unwind_threads(Session& session) {
  int threads = dwfl_getthreads(session.dwfl, thread_callback, &session);
  if (threads < 0) {
    const std::string message = "cannot enumerate threads: " + dwfl_error();
    if (session.live_pid > 0) {
      session.backtrace.warnings.push_back(
          {"live-thread-gone", {}, message + " (the process may have exited)"});
    } else {
      session.backtrace.warnings.push_back({"unwind-failed", {}, message});
    }
  }
}

Backtrace finalize(Session& session) {
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

} // namespace

Backtrace symbolize_core(const Options& options, CoreFile& core, NixResolver& resolver,
                         SourceResolver& sources, DebuginfodClient* debuginfod) {
  Session session;
  session.options = &options;
  session.resolver = &resolver;
  session.sources = &sources;
  session.debuginfod = debuginfod;
  session.backtrace.core.path = core.path;
  session.backtrace.core.compressed = core.compressed;

  if (!init_session(session, false, false)) {
    return finalize(session);
  }
  int reported = dwfl_core_file_report(session.dwfl, core.elf, nullptr);
  if (reported < 0) {
    session.backtrace.warnings.push_back(
        {"core-unreadable", {}, std::string("cannot read core modules: ") + dwfl_errmsg(-1)});
    return finalize(session);
  }
  dwfl_getmodules(session.dwfl, collect_module_callback, &session, 0);
  resolve_pending(session);
  configure_debuginfo_path(session);

  int pid = dwfl_core_file_attach(session.dwfl, core.elf);
  if (pid < 0) {
    session.backtrace.warnings.push_back(
        {"core-unreadable", {}, std::string("cannot attach core state: ") + dwfl_errmsg(-1)});
    return finalize(session);
  }
  if (session.backtrace.core.pid <= 0) {
    session.backtrace.core.pid = pid;
  }
  unwind_threads(session);
  return finalize(session);
}

Backtrace symbolize_pid(const Options& options, int pid, NixResolver& resolver,
                        SourceResolver& sources, DebuginfodClient* debuginfod) {
  Session session;
  session.options = &options;
  session.resolver = &resolver;
  session.sources = &sources;
  session.debuginfod = debuginfod;
  session.backtrace.core.path = "pid:" + std::to_string(pid);
  session.backtrace.core.pid = pid;
  session.live_pid = pid;
  session.backtrace.core.exe = read_link("/proc/" + std::to_string(pid) + "/exe");

  if (!init_session(session, false, true)) {
    return finalize(session);
  }
  int reported = dwfl_linux_proc_report(session.dwfl, pid);
  if (reported != 0) {
    session.backtrace.warnings.push_back(
        {"live-attach-failed",
         {},
         std::string("cannot report process modules: ") + dwfl_errmsg(-1)});
    return finalize(session);
  }
  if (dwfl_report_end(session.dwfl, nullptr, nullptr) != 0) {
    session.backtrace.warnings.push_back(
        {"live-attach-failed", {}, "cannot finish reporting process modules"});
    return finalize(session);
  }

  int attached = dwfl_linux_proc_attach(session.dwfl, pid, false);
  if (attached != 0) {
    session.backtrace.warnings.push_back(
        {"live-attach-failed", {}, std::string("cannot attach to process: ") + dwfl_errmsg(-1)});
    return finalize(session);
  }
  dwfl_getmodules(session.dwfl, collect_module_callback, &session, 0);
  resolve_pending(session);
  configure_debuginfo_path(session);
  unwind_threads(session);
  return finalize(session);
}

// Symbolize the addresses in a --stack description. Records are normally
// module-relative (link-time) addresses because every reported ELF uses
// dwfl_report_elf(base 0, add_p_vaddr=true). Two forms need extra handling:
//
//  * Runtime-address records ("MODULE RUNTIME @ LOAD_BASE" and
//    "BUILD-ID RUNTIME @ LOAD_BASE") keep the address the user supplied in
//    frame.pc and put the computed link-time address (runtime - load_base) in
//    frame.lookup_pc. The JSON "pc" field therefore echoes the stack file and
//    "lookup_pc" is the adjusted address, matching the meaning of those fields
//    for core and live backtraces.
//  * Build-id-only records have no module path. Each unique build id is
//    resolved to an executable through debuginfod before modules are reported.
//    When debuginfod is disabled or the fetch fails, a
//    "stack-build-id-unresolved" warning is recorded and that record is
//    skipped; the remaining records still symbolize.
Backtrace symbolize_stack(const Options& options, const std::vector<StackRecord>& records,
                          NixResolver& resolver, SourceResolver& sources,
                          DebuginfodClient* debuginfod) {
  Session session;
  session.options = &options;
  session.resolver = &resolver;
  session.sources = &sources;
  session.debuginfod = debuginfod;
  session.backtrace.core.path = options.stack;
  if (!init_session(session, true, false)) {
    return finalize(session);
  }

  // Resolve build-id-only records to local executables before reporting any
  // module. Failures are warnings, never fatal.
  std::unordered_map<std::string, std::string> executable_paths;
  for (const auto& record : records) {
    if (!record.module.empty() || record.build_id.empty()) {
      continue;
    }
    if (executable_paths.find(record.build_id) != executable_paths.end()) {
      continue;
    }
    if (debuginfod == nullptr || !debuginfod->enabled()) {
      session.backtrace.warnings.push_back(
          {"stack-build-id-unresolved", record.build_id,
           "cannot resolve a build-id-only stack record: debuginfod is disabled"});
      continue;
    }
    DebuginfodResult fetched = debuginfod->find_executable(record.build_id);
    if (!fetched.ok) {
      session.backtrace.warnings.push_back(
          {"stack-build-id-unresolved", record.build_id,
           "cannot fetch the executable for build id: " + fetched.error});
      continue;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fetched.path, ec)) {
      session.backtrace.warnings.push_back(
          {"stack-build-id-unresolved", record.build_id,
           "debuginfod returned a non-regular executable path: " + fetched.path});
      continue;
    }
    executable_paths[record.build_id] = fetched.path;
  }

  dwfl_report_begin(session.dwfl);
  std::unordered_map<std::string, Dwfl_Module*> reported_modules;
  std::vector<std::string> record_paths(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    const StackRecord& record = records[index];
    std::string module_path;
    if (!record.module.empty()) {
      module_path = resolve_relative_path(record.module, options.cwd);
    } else {
      const auto resolved = executable_paths.find(record.build_id);
      if (resolved == executable_paths.end()) {
        continue; // A stack-build-id-unresolved warning was recorded above.
      }
      module_path = resolved->second;
    }
    record_paths[index] = module_path;
    if (reported_modules.find(module_path) != reported_modules.end()) {
      continue;
    }

    const bool from_build_id = record.module.empty();
    const std::string warning_module = from_build_id ? record.build_id : record.module;
    const char* warning_code = from_build_id ? "stack-build-id-unresolved" : "module-file-missing";
    const auto report_failure = [&](const std::string& message) {
      session.backtrace.warnings.push_back({warning_code, warning_module, message});
    };

    std::error_code ec;
    if (!std::filesystem::exists(module_path, ec)) {
      report_failure("stack module does not exist: " + module_path);
      continue;
    }
    int fd = open(module_path.c_str(), O_RDONLY);
    if (fd < 0) {
      report_failure("cannot open stack module: " + module_path);
      continue;
    }
    std::string module_name = std::filesystem::path(module_path).filename().string();
    Dwfl_Module* module =
        dwfl_report_elf(session.dwfl, module_name.c_str(), module_path.c_str(), fd, 0, true);
    if (module == nullptr) {
      close(fd);
      report_failure("cannot report stack module: " + dwfl_error());
      continue;
    }
    reported_modules[module_path] = module;
  }
  if (dwfl_report_end(session.dwfl, nullptr, nullptr) != 0) {
    session.backtrace.warnings.push_back(
        {"core-unreadable", {}, "cannot finish reporting stack modules"});
  }
  dwfl_getmodules(session.dwfl, collect_module_callback, &session, 0);
  resolve_pending(session);
  configure_debuginfo_path(session);

  ThreadInfo thread;
  thread.tid = 0;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const StackRecord& record = records[index];
    if (record_paths[index].empty()) {
      continue;
    }
    const auto found = reported_modules.find(record_paths[index]);
    if (found == reported_modules.end()) {
      continue;
    }
    std::uint64_t lookup_address = 0;
    if (!stack_record_lookup_address(record, &lookup_address)) {
      session.backtrace.warnings.push_back(
          {"stack-address-underflow", record.module.empty() ? record.build_id : record.module,
           "runtime address " + hex_value(record.address) + " is below load base " +
               hex_value(record.load_base) + "; skipping record"});
      continue;
    }
    Dwfl_Module* module = found->second;
    Frame frame;
    frame.pc = record.address;
    frame.lookup_pc = lookup_address;
    ModuleInfo* module_info = module_for(session, module);
    if (module_info != nullptr) {
      frame.module_path = module_info->path;
      frame.module_name = module_info->name;
      frame.build_id = module_info->build_id;
    }
    symbolize_address(session, module, lookup_address, module_info, &frame);
    if (session.has_filter) {
      const std::string& name = frame.demangled.empty() ? frame.mangled : frame.demangled;
      if (!std::regex_search(name, session.filter)) {
        continue;
      }
    }
    frame.index = static_cast<unsigned>(thread.frames.size());
    thread.frames.push_back(std::move(frame));
  }
  session.backtrace.threads.push_back(std::move(thread));
  return finalize(session);
}

} // namespace nix_sym
