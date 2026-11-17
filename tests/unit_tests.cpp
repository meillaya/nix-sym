#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "demangle.hpp"
#include "gdb.hpp"
#include "json.hpp"
#include "model.hpp"
#include "source.hpp"
#include "test.hpp"

namespace {

using nix_sym::Backtrace;
using nix_sym::Frame;
using nix_sym::Language;
using nix_sym::ModuleInfo;
using nix_sym::ModuleStatus;
using nix_sym::Options;
using nix_sym::SourceLocation;
using nix_sym::ThreadInfo;
using nix_sym::Warning;
using nix_sym_test::failures;

void test_json_escape() {
  CHECK_EQ(nix_sym::json_escape("plain"), "plain");
  CHECK_EQ(nix_sym::json_escape("a\"b"), "a\\\"b");
  CHECK_EQ(nix_sym::json_escape("a\\b"), "a\\\\b");
  CHECK_EQ(nix_sym::json_escape("a\nb"), "a\\nb");
  CHECK_EQ(nix_sym::json_escape(std::string("a\001b")), "a\\u0001b");
}

void test_languages() {
  CHECK(nix_sym::language_from_dwarf(0x0004) == Language::Cxx);
  CHECK(nix_sym::language_from_dwarf(0x001c) == Language::Rust);
  CHECK(nix_sym::language_from_dwarf(0x0018) == Language::Haskell);
  CHECK(nix_sym::language_from_dwarf(0x0002) == Language::C);
  CHECK(nix_sym::language_from_symbol("_RNvNtC3std3mem8align_ofj") == Language::Rust);
  CHECK(nix_sym::language_from_symbol("_ZN4core3fmt5write17h0123456789abcdefE") == Language::Rust);
  CHECK(nix_sym::language_from_symbol("_ZN3foo3barEv") == Language::Cxx);
  CHECK(nix_sym::language_from_symbol("Main_main_info") == Language::C);
  CHECK(nix_sym::language_from_symbol("GHCziIO_putStrLn_info") == Language::Haskell);
}

void test_cxx_demangle() {
  CHECK_EQ(nix_sym::demangle_cxx("_ZN3foo3barEv"), "foo::bar()");
  CHECK_EQ(nix_sym::demangle_cxx("_ZN3foo3barEv.cold"), "foo::bar().cold");
  CHECK_EQ(nix_sym::demangle_cxx("plain_c_symbol"), "plain_c_symbol");
  CHECK_EQ(nix_sym::demangle_cxx("_Z3foov@@GLIBC_2.2.5"), "foo()");
}

void test_json_render() {
  Backtrace backtrace;
  backtrace.core.path = "/tmp/core";
  backtrace.core.pid = 42;
  backtrace.core.signal = 11;
  backtrace.core.signal_name = "SIGSEGV";
  ModuleInfo module;
  module.name = "demo";
  module.path = "/nix/store/abc-demo/bin/demo";
  module.build_id = "0011223344";
  module.status = ModuleStatus::MissingDebug;
  backtrace.modules.push_back(module);
  ThreadInfo thread;
  thread.tid = 7;
  Frame frame;
  frame.index = 0;
  frame.pc = 0x1234;
  frame.module_path = module.path;
  frame.module_name = module.name;
  frame.mangled = "_Z3foov";
  frame.demangled = "foo()";
  frame.language = "c++";
  frame.has_symbol = true;
  frame.location.valid = true;
  frame.location.original = "/build/demo/main.cpp";
  frame.location.line = 12;
  frame.location.comp_dir = "/build/demo";
  frame.location.status = "available";
  thread.frames.push_back(frame);
  backtrace.threads.push_back(thread);
  backtrace.partial = true;
  std::string json = nix_sym::render_json(backtrace);
  CHECK(json.find("\"schema\": 1") != std::string::npos);
  CHECK(json.find("\"status\": \"partial\"") != std::string::npos);
  CHECK(json.find("\"pid\": 42") != std::string::npos);
  CHECK(json.find("\"mangled\": \"_Z3foov\"") != std::string::npos);
  CHECK(json.find("\"original\": \"/build/demo/main.cpp\"") != std::string::npos);
  CHECK(json.find("\"code\": \"") == std::string::npos);
}

void test_gdb_render() {
  Backtrace backtrace;
  ModuleInfo module;
  module.debug_output = "/nix/store/xyz-demo-debug";
  backtrace.modules.push_back(module);
  nix_sym::SourceMapping mapping;
  mapping.module = "/nix/store/abc-demo";
  mapping.comp_dir = "/build/demo";
  mapping.root = "demo";
  mapping.resolved_root = "/cache/sources/demo";
  mapping.status = "resolved";
  std::string gdb = nix_sym::render_gdb_init(backtrace, "/tmp/core", {mapping}, false);
  CHECK(gdb.find("set pagination off") != std::string::npos);
  CHECK(gdb.find("set debuginfod enabled off") != std::string::npos);
  CHECK(gdb.find("core-file /tmp/core") != std::string::npos);
  CHECK(gdb.find("set substitute-path /build/demo /cache/sources/demo") != std::string::npos);

  Backtrace live;
  live.core.pid = 4242;
  std::string live_gdb = nix_sym::render_gdb_init(live, "", {mapping}, true);
  CHECK(live_gdb.find("attach 4242") != std::string::npos);
  CHECK(live_gdb.find("core-file") == std::string::npos);
}

void test_cli() {
  {
    std::vector<std::string> args = {"nix-sym", "--core", "/tmp/core", "--json", "--bt"};
    std::vector<char*> argv;
    for (auto& arg : args)
      argv.push_back(arg.data());
    Options options = nix_sym::parse_cli(static_cast<int>(argv.size()), argv.data());
    CHECK(options.core == "/tmp/core");
    CHECK(options.json);
    CHECK(options.bt);
    CHECK(!options.help);
  }
  {
    std::vector<std::string> args = {"nix-sym", "--core=/tmp/core", "--max-frames=12", "--thread",
                                     "5",       "--offline"};
    std::vector<char*> argv;
    for (auto& arg : args)
      argv.push_back(arg.data());
    Options options = nix_sym::parse_cli(static_cast<int>(argv.size()), argv.data());
    CHECK(options.core == "/tmp/core");
    CHECK_EQ(options.max_frames, 12);
    CHECK_EQ(options.threads.size(), static_cast<size_t>(1));
    CHECK_EQ(options.threads[0], 5);
    CHECK(!options.fetch);
    CHECK(options.offline);
  }
  {
    std::vector<std::string> args = {"nix-sym"};
    std::vector<char*> argv;
    for (auto& arg : args)
      argv.push_back(arg.data());
    bool threw = false;
    try {
      nix_sym::parse_cli(static_cast<int>(argv.size()), argv.data());
    } catch (const nix_sym::UsageError&) {
      threw = true;
    }
    CHECK(threw);
  }
}

void test_source_mapping() {
  nix_sym::SourceResolver resolver("/tmp/nix-sym-test-cache", false, false);
  ModuleInfo module;
  module.path = "/nix/store/abc-demo";
  module.source_path = "/nix/store/xyz-source";
  SourceLocation location = resolver.map(module, "/build/demo-1.0", "/build/demo-1.0/src/main.cpp");
  CHECK(location.valid);
  CHECK_EQ(location.original, "/build/demo-1.0/src/main.cpp");
  CHECK(location.status != "resolved");
  CHECK_EQ(resolver.mappings().size(), static_cast<size_t>(1));
}

void test_source_maps() {
  namespace fs = std::filesystem;
  fs::path directory = fs::temp_directory_path() / "nix-sym-source-map-test";
  fs::create_directories(directory / "src");
  {
    std::ofstream output(directory / "src/main.cpp");
    output << "int main() { return 0; }\n";
  }
  nix_sym::SourceResolver resolver("/tmp/nix-sym-test-cache", false, false,
                                   {"/build/demo=" + directory.string()}, nullptr);
  nix_sym::ModuleInfo module;
  module.path = "/nix/store/abc-demo";
  module.build_id = "00112233";
  nix_sym::SourceLocation location =
      resolver.map(module, "/build/demo", "/build/demo/src/main.cpp");
  CHECK(location.valid);
  CHECK_EQ(location.status, "resolved");
  CHECK_EQ(location.resolved, (directory / "src/main.cpp").string());
  CHECK_EQ(resolver.mappings().size(), static_cast<size_t>(1));
  fs::remove_all(directory);
}

void test_models() {
  CHECK_EQ(std::string(nix_sym::module_status_name(ModuleStatus::Resolved)), "resolved");
  CHECK_EQ(std::string(nix_sym::module_status_name(ModuleStatus::MissingDebug)), "missing-debug");
  CHECK_EQ(std::string(nix_sym::module_status_name(ModuleStatus::NonNix)), "non-nix");
}

} // namespace

int main() {
  test_json_escape();
  test_languages();
  test_cxx_demangle();
  test_json_render();
  test_gdb_render();
  test_cli();
  test_source_mapping();
  test_source_maps();
  test_models();
  if (failures == 0) {
    std::cout << "unit tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
