#include "../test.hpp"

#include "debuginfod.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

using nix_sym::DebuginfodClient;
using nix_sym::DebuginfodResult;

int checks = 0;
int groups = 0;

#define CHECK_COUNTED(expr)                                                                        \
  do {                                                                                             \
    ++checks;                                                                                      \
    CHECK(expr);                                                                                   \
  } while (false)

#define CHECK_EQ_COUNTED(actual, expected)                                                         \
  do {                                                                                             \
    ++checks;                                                                                      \
    CHECK_EQ((actual), (expected));                                                                \
  } while (false)

class ScopedEnv {
public:
  explicit ScopedEnv(const char* name) : name_(name) {
    const char* value = std::getenv(name_.c_str());
    had_value_ = value != nullptr;
    if (had_value_) {
      old_value_ = value;
    }
  }

  ~ScopedEnv() {
    if (had_value_) {
      ::setenv(name_.c_str(), old_value_.c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

  static void unset(const char* name) {
    ::unsetenv(name);
  }

private:
  std::string name_;
  std::string old_value_;
  bool had_value_ = false;
};

struct TempRoot {
  fs::path path;

  TempRoot() {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
      return;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      fs::path candidate = base / ("nix-sym-debuginfod-test-" + std::to_string(stamp) + "-" +
                                   std::to_string(attempt));
      std::error_code create_error;
      if (fs::create_directory(candidate, create_error) && !create_error) {
        path = std::move(candidate);
        return;
      }
    }
  }

  ~TempRoot() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  }

  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
};

bool write_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  return static_cast<bool>(out);
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string content;
  char buffer[4096];
  while (in) {
    in.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
    const std::streamsize count = in.gcount();
    if (count > 0) {
      content.append(buffer, static_cast<std::size_t>(count));
    }
  }
  return content;
}

constexpr const char* kBuildId = "0123456789abcdef0123456789abcdef01234567";
constexpr const char* kUpperCaseBuildId = "0123456789ABCDEF0123456789ABCDEF01234567";

void test_offline_disabled(const fs::path& root) {
  ++groups;
  ScopedEnv env("DEBUGINFOD_URLS");
  ScopedEnv::unset("DEBUGINFOD_URLS");

  DebuginfodClient client(root.string(), {"https://debuginfod.invalid"}, true);
  CHECK_COUNTED(!client.enabled());
  CHECK_EQ_COUNTED(client.urls().size(), std::size_t{1});
  CHECK_EQ_COUNTED(client.urls().front(), std::string("https://debuginfod.invalid"));

  const DebuginfodResult debug = client.find_debuginfo(kBuildId);
  CHECK_COUNTED(!debug.ok);
  CHECK_COUNTED(!debug.error.empty());

  const DebuginfodResult source = client.find_source(kBuildId, "/build/foo/a.c");
  CHECK_COUNTED(!source.ok);
  CHECK_COUNTED(!source.error.empty());
}

void test_empty_urls_disabled(const fs::path& root) {
  ++groups;
  ScopedEnv env("DEBUGINFOD_URLS");
  ScopedEnv::unset("DEBUGINFOD_URLS");

  DebuginfodClient client(root.string(), {}, false);
  CHECK_COUNTED(!client.enabled());
  CHECK_COUNTED(client.urls().empty());

  const DebuginfodResult result = client.find_debuginfo(kBuildId);
  CHECK_COUNTED(!result.ok);
  CHECK_COUNTED(!result.error.empty());
}

void test_explicit_urls_enabled(const fs::path& root) {
  ++groups;
  ScopedEnv env("DEBUGINFOD_URLS");
  ScopedEnv::unset("DEBUGINFOD_URLS");

  const std::vector<std::string> urls = {"https://debuginfod.invalid", "http://127.0.0.1:8002"};
  DebuginfodClient client(root.string(), urls, false);
  CHECK_COUNTED(client.enabled());
  CHECK_EQ_COUNTED(client.urls().size(), std::size_t{2});
  CHECK_EQ_COUNTED(client.urls().at(0), urls.at(0));
  CHECK_EQ_COUNTED(client.urls().at(1), urls.at(1));

  const char* environment_value = std::getenv("DEBUGINFOD_URLS");
  CHECK_COUNTED(environment_value != nullptr);
  if (environment_value != nullptr) {
    CHECK_EQ_COUNTED(std::string(environment_value),
                     std::string("https://debuginfod.invalid http://127.0.0.1:8002"));
  }
}

void test_urls_from_environment(const fs::path& root) {
  ++groups;
  ScopedEnv env("DEBUGINFOD_URLS");
  ScopedEnv::unset("DEBUGINFOD_URLS");
  ::setenv("DEBUGINFOD_URLS", "https://debuginfod.invalid http://127.0.0.1:8002", 1);

  DebuginfodClient client(root.string(), {}, false);
  CHECK_COUNTED(client.enabled());
  CHECK_EQ_COUNTED(client.urls().size(), std::size_t{2});
  CHECK_EQ_COUNTED(client.urls().at(0), std::string("https://debuginfod.invalid"));
  CHECK_EQ_COUNTED(client.urls().at(1), std::string("http://127.0.0.1:8002"));
}

void test_invalid_build_ids(const fs::path& root) {
  ++groups;
  ScopedEnv env("DEBUGINFOD_URLS");
  ScopedEnv::unset("DEBUGINFOD_URLS");

  DebuginfodClient client(root.string(), {"http://127.0.0.1:8002"}, false);
  CHECK_COUNTED(client.enabled());

  const fs::path fetched = root / "invalid-id.debug";
  CHECK_COUNTED(write_file(fetched, "payload"));

  for (const std::string& build_id : std::vector<std::string>{"", "abc", "zz", "0"}) {
    const DebuginfodResult debug = client.find_debuginfo(build_id);
    CHECK_COUNTED(!debug.ok);
    CHECK_COUNTED(debug.error.find("build id") != std::string::npos);

    const DebuginfodResult source = client.find_source(build_id, "/build/foo/a.c");
    CHECK_COUNTED(!source.ok);
    CHECK_COUNTED(source.error.find("build id") != std::string::npos);

    std::string error;
    CHECK_EQ_COUNTED(client.install_debuginfo(build_id, fetched.string(), &error), std::string());
    CHECK_COUNTED(!error.empty());
  }
}

void test_install_debuginfo_layout_and_idempotence(const fs::path& root) {
  ++groups;
  ScopedEnv env("DEBUGINFOD_URLS");
  ScopedEnv::unset("DEBUGINFOD_URLS");

  const fs::path cache_root = root / "cache-debuginfo";
  DebuginfodClient client(cache_root.string(), {"http://127.0.0.1:8002"}, false);
  CHECK_COUNTED(!fs::exists(cache_root / "debuginfod"));

  const fs::path fetched = root / "fetched.debug";
  CHECK_COUNTED(write_file(fetched, "first debug payload\n"));

  const std::string lower = kBuildId;
  const fs::path expected_output = cache_root / "debuginfod" / lower;
  const fs::path destination = expected_output / "lib" / "debug" / ".build-id" /
                               lower.substr(0, 2) / (lower.substr(2) + ".debug");

  std::string error = "sentinel";
  const std::string output = client.install_debuginfo(kUpperCaseBuildId, fetched.string(), &error);
  CHECK_EQ_COUNTED(output, expected_output.string());
  CHECK_COUNTED(error.empty());
  CHECK_COUNTED(fs::is_regular_file(destination));
  CHECK_EQ_COUNTED(read_file(destination), std::string("first debug payload\n"));

  CHECK_COUNTED(write_file(fetched, "changed debug payload\n"));
  error = "sentinel";
  const std::string second = client.install_debuginfo(lower, fetched.string(), &error);
  CHECK_EQ_COUNTED(second, expected_output.string());
  CHECK_COUNTED(error.empty());
  CHECK_EQ_COUNTED(read_file(destination), std::string("first debug payload\n"));

  CHECK_EQ_COUNTED(client.install_debuginfo(lower, fetched.string(), nullptr),
                   expected_output.string());

  std::string missing_error;
  CHECK_EQ_COUNTED(
      client.install_debuginfo(lower, (root / "missing.debug").string(), &missing_error),
      std::string());
  CHECK_COUNTED(!missing_error.empty());
}

void test_install_source_mirror_and_rejections(const fs::path& root) {
  ++groups;
  ScopedEnv env("DEBUGINFOD_URLS");
  ScopedEnv::unset("DEBUGINFOD_URLS");

  const fs::path cache_root = root / "cache-source";
  DebuginfodClient client(cache_root.string(), {"http://127.0.0.1:8002"}, false);
  CHECK_EQ_COUNTED(client.source_root(), (cache_root / "debuginfod-sources").string());

  const fs::path fetched = root / "a.c";
  CHECK_COUNTED(write_file(fetched, "int first;\n"));

  const fs::path expected = cache_root / "debuginfod-sources" / "build" / "foo" / "a.c";
  CHECK_EQ_COUNTED(client.install_source(kBuildId, "/build/foo/a.c", fetched.string()),
                   expected.string());
  CHECK_EQ_COUNTED(read_file(expected), std::string("int first;\n"));
  CHECK_COUNTED(fs::exists(cache_root / "debuginfod-sources"));

  CHECK_COUNTED(write_file(fetched, "int second;\n"));
  CHECK_EQ_COUNTED(client.install_source(kBuildId, "/build/foo/a.c", fetched.string()),
                   expected.string());
  CHECK_EQ_COUNTED(read_file(expected), std::string("int second;\n"));

  CHECK_EQ_COUNTED(client.install_source(kBuildId, "", fetched.string()), std::string());
  CHECK_EQ_COUNTED(client.install_source(kBuildId, "build/foo/a.c", fetched.string()),
                   std::string());
  CHECK_EQ_COUNTED(client.install_source(kBuildId, "/build/../etc/passwd", fetched.string()),
                   std::string());
  CHECK_EQ_COUNTED(client.install_source(kBuildId, "/build/../../etc/passwd", fetched.string()),
                   std::string());
  CHECK_EQ_COUNTED(client.install_source(kBuildId, "//etc/passwd", fetched.string()),
                   std::string());
  CHECK_COUNTED(!fs::exists(cache_root / "debuginfod-sources" / "etc" / "passwd"));

  CHECK_EQ_COUNTED(client.install_source(kBuildId, "/build/foo/a.c", ""), std::string());
  CHECK_EQ_COUNTED(client.install_source(kBuildId, "/build/foo/a.c", (root / "missing.c").string()),
                   std::string());
}

} // namespace

int main() {
  std::cout << "debuginfod_test\n";

  TempRoot temp;
  if (temp.path.empty()) {
    std::cerr << "could not create a temporary directory\n";
    return 1;
  }

  test_offline_disabled(temp.path);
  test_empty_urls_disabled(temp.path);
  test_explicit_urls_enabled(temp.path);
  test_urls_from_environment(temp.path);
  test_invalid_build_ids(temp.path);
  test_install_debuginfo_layout_and_idempotence(temp.path);
  test_install_source_mirror_and_rejections(temp.path);

  std::cout << "\n"
            << groups << " test groups, " << checks << " checks, " << nix_sym_test::failures
            << " failure(s)\n";
  return nix_sym_test::failures == 0 ? 0 : 1;
}
