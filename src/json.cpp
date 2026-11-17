#include "json.hpp"

#include <cstdio>
#include <sstream>
#include <string>

#include "demangle.hpp"

namespace nix_sym {
namespace {

std::string hex(std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
  return buffer;
}

std::string quote(const std::string& value) {
  return "\"" + json_escape(value) + "\"";
}

void render_source(std::ostringstream& out, const SourceLocation& source, int indent) {
  std::string pad(static_cast<size_t>(indent), ' ');
  if (!source.valid) {
    out << "null";
    return;
  }
  out << "{\n";
  out << pad << "  \"original\": " << quote(source.original) << ",\n";
  out << pad << "  \"comp_dir\": " << quote(source.comp_dir) << ",\n";
  out << pad << "  \"line\": " << source.line << ",\n";
  out << pad << "  \"column\": " << source.column << ",\n";
  out << pad << "  \"resolved\": " << (source.resolved.empty() ? "null" : quote(source.resolved))
      << ",\n";
  out << pad << "  \"status\": " << quote(source.status) << "\n";
  out << pad << "}";
}

void render_function(std::ostringstream& out, const Frame& frame, int indent) {
  std::string pad(static_cast<size_t>(indent), ' ');
  out << "{\n";
  out << pad << "  \"mangled\": " << quote(frame.mangled) << ",\n";
  out << pad << "  \"demangled\": " << quote(frame.demangled) << ",\n";
  out << pad << "  \"language\": " << quote(frame.language) << "\n";
  out << pad << "}";
}

} // namespace

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
        out << buffer;
      } else {
        out << static_cast<char>(c);
      }
    }
  }
  return out.str();
}

std::string render_json(const Backtrace& backtrace) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"schema\": 1,\n";
  out << "  \"tool\": {\"name\": \"nix-sym\", \"version\": \"0.2.0\"},\n";
  out << "  \"status\": " << quote(backtrace.partial ? "partial" : "complete") << ",\n";

  out << "  \"core\": {\n";
  out << "    \"path\": " << quote(backtrace.core.path) << ",\n";
  out << "    \"compressed\": " << (backtrace.core.compressed ? "true" : "false") << ",\n";
  out << "    \"pid\": " << backtrace.core.pid << ",\n";
  out << "    \"exe\": " << (backtrace.core.exe.empty() ? "null" : quote(backtrace.core.exe))
      << ",\n";
  out << "    \"signal\": {\"number\": " << backtrace.core.signal << ", \"name\": "
      << (backtrace.core.signal_name.empty() ? "null" : quote(backtrace.core.signal_name))
      << "},\n";
  out << "    \"truncated\": " << (backtrace.core.truncated ? "true" : "false") << "\n";
  out << "  },\n";

  out << "  \"modules\": [\n";
  for (size_t i = 0; i < backtrace.modules.size(); ++i) {
    const ModuleInfo& module = backtrace.modules[i];
    out << "    {\n";
    out << "      \"path\": " << quote(module.path) << ",\n";
    out << "      \"name\": " << quote(module.name) << ",\n";
    out << "      \"build_id\": " << (module.build_id.empty() ? "null" : quote(module.build_id))
        << ",\n";
    out << "      \"status\": " << quote(module_status_name(module.status)) << ",\n";
    out << "      \"debuginfod\": " << (module.debuginfod ? "true" : "false") << ",\n";
    out << "      \"drv\": " << (module.drv_path.empty() ? "null" : quote(module.drv_path))
        << ",\n";
    out << "      \"debug_output\": "
        << (module.debug_output.empty() ? "null" : quote(module.debug_output)) << ",\n";
    out << "      \"debug_file\": "
        << (module.debug_file.empty() ? "null" : quote(module.debug_file)) << ",\n";
    out << "      \"source\": {\n";
    out << "        \"kind\": " << quote(module.source_kind) << ",\n";
    out << "        \"path\": " << (module.source_path.empty() ? "null" : quote(module.source_path))
        << ",\n";
    out << "        \"overlay\": "
        << (module.overlay_path.empty() ? "null" : quote(module.overlay_path)) << ",\n";
    out << "        \"root\": " << (module.source_root.empty() ? "null" : quote(module.source_root))
        << ",\n";
    out << "        \"status\": " << quote(module.source_status) << "\n";
    out << "      }\n";
    out << "    }" << (i + 1 == backtrace.modules.size() ? "\n" : ",\n");
  }
  out << "  ],\n";

  out << "  \"threads\": [\n";
  for (size_t t = 0; t < backtrace.threads.size(); ++t) {
    const ThreadInfo& thread = backtrace.threads[t];
    out << "    {\n";
    out << "      \"tid\": " << thread.tid << ",\n";
    out << "      \"frames\": [\n";
    for (size_t f = 0; f < thread.frames.size(); ++f) {
      const Frame& frame = thread.frames[f];
      out << "        {\n";
      out << "          \"index\": " << frame.index << ",\n";
      out << "          \"pc\": " << quote(hex(frame.pc)) << ",\n";
      out << "          \"lookup_pc\": " << quote(hex(frame.lookup_pc)) << ",\n";
      out << "          \"activation\": " << (frame.activation ? "true" : "false") << ",\n";
      out << "          \"module\": "
          << (frame.module_path.empty() ? "null" : quote(frame.module_path)) << ",\n";
      out << "          \"module_name\": " << quote(frame.module_name) << ",\n";
      out << "          \"build_id\": " << (frame.build_id.empty() ? "null" : quote(frame.build_id))
          << ",\n";
      out << "          \"function\": ";
      render_function(out, frame, 10);
      out << ",\n";
      out << "          \"source\": ";
      render_source(out, frame.location, 10);
      out << ",\n";
      out << "          \"inline\": [\n";
      for (size_t i = 0; i < frame.inline_frames.size(); ++i) {
        const InlineFrame& inline_frame = frame.inline_frames[i];
        out << "            {\n";
        out << "              \"mangled\": " << quote(inline_frame.mangled) << ",\n";
        out << "              \"demangled\": " << quote(inline_frame.demangled) << ",\n";
        out << "              \"language\": " << quote(inline_frame.language) << ",\n";
        out << "              \"call\": ";
        render_source(out, inline_frame.call, 14);
        out << "\n";
        out << "            }" << (i + 1 == frame.inline_frames.size() ? "\n" : ",\n");
      }
      out << "          ]\n";
      out << "        }" << (f + 1 == thread.frames.size() ? "\n" : ",\n");
    }
    out << "      ]\n";
    out << "    }" << (t + 1 == backtrace.threads.size() ? "\n" : ",\n");
  }
  out << "  ],\n";

  out << "  \"warnings\": [\n";
  for (size_t i = 0; i < backtrace.warnings.size(); ++i) {
    const Warning& warning = backtrace.warnings[i];
    out << "    {\"code\": " << quote(warning.code)
        << ", \"module\": " << (warning.module.empty() ? "null" : quote(warning.module))
        << ", \"message\": " << quote(warning.message) << "}"
        << (i + 1 == backtrace.warnings.size() ? "\n" : ",\n");
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

std::string render_text(const Backtrace& backtrace, bool color, bool demangle) {
  (void)demangle;
  const char* bold = color ? "\033[1m" : "";
  const char* dim = color ? "\033[2m" : "";
  const char* reset = color ? "\033[0m" : "";
  std::ostringstream out;
  out << bold << "Core " << reset << backtrace.core.path;
  if (backtrace.core.pid > 0) {
    out << " (pid " << backtrace.core.pid << ")";
  }
  if (!backtrace.core.exe.empty()) {
    out << " executable " << backtrace.core.exe;
  }
  if (!backtrace.core.signal_name.empty()) {
    out << " signal " << backtrace.core.signal_name;
  }
  out << "\n";
  out << dim << backtrace.modules.size() << " modules, " << backtrace.threads.size() << " threads, "
      << backtrace.warnings.size() << " warnings" << reset << "\n";

  for (const auto& thread : backtrace.threads) {
    out << "\n" << bold << "Thread " << thread.tid << reset << "\n";
    if (thread.frames.empty()) {
      out << "  (no frames)\n";
      continue;
    }
    for (const auto& frame : thread.frames) {
      const std::string& name = frame.demangled.empty() ? frame.mangled : frame.demangled;
      out << "  #" << frame.index << "  " << hex(frame.pc) << "  "
          << (name.empty() ? "<unknown>" : name);
      if (!frame.module_name.empty()) {
        out << dim << "  [" << frame.module_name << "]" << reset;
      }
      out << "\n";
      if (frame.location.valid) {
        out << "      at " << frame.location.original;
        if (frame.location.line > 0) {
          out << ":" << frame.location.line;
        }
        if (frame.location.column > 0) {
          out << ":" << frame.location.column;
        }
        if (!frame.location.resolved.empty()) {
          out << dim << "  -> " << frame.location.resolved << reset;
        }
        out << "\n";
      }
      for (const auto& inline_frame : frame.inline_frames) {
        const std::string& inline_name =
            inline_frame.demangled.empty() ? inline_frame.mangled : inline_frame.demangled;
        out << "      inlined by " << inline_name;
        if (inline_frame.call.valid) {
          out << " at " << inline_frame.call.original;
          if (inline_frame.call.line > 0) {
            out << ":" << inline_frame.call.line;
          }
        }
        out << "\n";
      }
    }
  }

  if (!backtrace.warnings.empty()) {
    out << "\n" << bold << "Warnings" << reset << "\n";
    for (const auto& warning : backtrace.warnings) {
      out << "  " << warning.code;
      if (!warning.module.empty()) {
        out << " " << warning.module;
      }
      if (!warning.message.empty()) {
        out << ": " << warning.message;
      }
      out << "\n";
    }
  }
  return out.str();
}

} // namespace nix_sym
