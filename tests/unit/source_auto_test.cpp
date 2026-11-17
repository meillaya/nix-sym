#include "../test.hpp"

#include "source.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

namespace fs = std::filesystem;

struct TempDir {
  fs::path path;
  TempDir() {
    std::error_code ec;
    path = fs::temp_directory_path(ec) / "nix-sym-source-auto-XXXXXX";
    if (ec) {
      return;
    }
    std::string pattern = path.string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = mkdtemp(buffer.data());
    if (created != nullptr) {
      path = created;
    }
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_file(const fs::path& path) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream output(path);
  output << "// test\n";
}

void test_rust_src_path_root() {
  TempDir temp;
  const fs::path root = temp.path / "rust";
  write_file(root / "library/core/src/ptr/mod.rs");
  setenv("RUST_SRC_PATH", root.string().c_str(), 1);

  nix_sym::SourceResolver resolver((temp.path / "cache").string(), false, false);
  nix_sym::ModuleInfo module;
  module.path = "/nix/store/abc-demo/bin/demo";
  module.name = "demo";
  nix_sym::SourceLocation location =
      resolver.map(module, "/rustc/58cf8f9c", "/rustc/58cf8f9c/library/core/src/ptr/mod.rs");
  CHECK(location.valid);
  CHECK_EQ(location.status, "resolved");
  CHECK_EQ(location.resolved, (root / "library/core/src/ptr/mod.rs").string());
  auto mappings = resolver.mappings();
  CHECK_EQ(mappings.size(), static_cast<size_t>(1));
  CHECK_EQ(mappings[0].comp_dir, "/rustc/58cf8f9c");
  CHECK_EQ(mappings[0].resolved_root, root.string());
}

void test_rust_src_path_library() {
  TempDir temp;
  const fs::path root = temp.path / "rust";
  write_file(root / "library/core/src/lib.rs");
  setenv("RUST_SRC_PATH", (root / "library").string().c_str(), 1);

  nix_sym::SourceResolver resolver((temp.path / "cache").string(), false, false);
  nix_sym::ModuleInfo module;
  module.name = "demo";
  nix_sym::SourceLocation location =
      resolver.map(module, "/rustc/deadbeef", "/rustc/deadbeef/library/core/src/lib.rs");
  CHECK_EQ(location.status, "resolved");
  CHECK_EQ(location.resolved, (root / "library/core/src/lib.rs").string());
}

void test_explicit_source_dir_wins() {
  TempDir temp;
  const fs::path rust_root = temp.path / "rust";
  const fs::path override_root = temp.path / "override";
  write_file(rust_root / "library/core/src/lib.rs");
  write_file(override_root / "library/core/src/lib.rs");
  setenv("RUST_SRC_PATH", rust_root.string().c_str(), 1);

  nix_sym::SourceResolver resolver((temp.path / "cache").string(), false, false,
                                   {"/rustc/feedface=" + override_root.string()}, nullptr);
  nix_sym::ModuleInfo module;
  module.name = "demo";
  nix_sym::SourceLocation location =
      resolver.map(module, "/rustc/feedface", "/rustc/feedface/library/core/src/lib.rs");
  CHECK_EQ(location.status, "resolved");
  CHECK_EQ(location.resolved, (override_root / "library/core/src/lib.rs").string());
}

void test_generated_file_status() {
  TempDir temp;
  const fs::path source = temp.path / "source";
  std::error_code ec;
  fs::create_directories(source / "src", ec);
  write_file(source / "src/main.c");
  unsetenv("RUST_SRC_PATH");

  nix_sym::SourceResolver resolver((temp.path / "cache").string(), false, false);
  nix_sym::ModuleInfo module;
  module.path = "/nix/store/abc-demo/bin/demo";
  module.name = "demo";
  module.source_path = source.string();
  nix_sym::SourceLocation location =
      resolver.map(module, "/build/demo", "/build/demo/generated/generated.c");
  CHECK_EQ(location.status, "source-generated-file");
  CHECK(location.resolved.empty());
}

void test_missing_rust_tree() {
  TempDir temp;
  unsetenv("RUST_SRC_PATH");
  setenv("NIX_SYM_RUST_SRC", (temp.path / "does-not-exist").string().c_str(), 1);
  nix_sym::SourceResolver resolver((temp.path / "cache").string(), false, false);
  nix_sym::ModuleInfo module;
  module.name = "demo";
  nix_sym::SourceLocation location =
      resolver.map(module, "/rustc/abc", "/rustc/abc/library/core/src/lib.rs");
  CHECK_EQ(location.status, "unavailable");
  unsetenv("NIX_SYM_RUST_SRC");
}

} // namespace

int main() {
  test_rust_src_path_root();
  test_rust_src_path_library();
  test_explicit_source_dir_wins();
  test_generated_file_status();
  test_missing_rust_tree();
  if (nix_sym_test::failures == 0) {
    std::cout << "source auto tests passed\n";
  }
  return nix_sym_test::failures == 0 ? 0 : 1;
}
