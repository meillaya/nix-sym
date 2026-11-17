#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../test.hpp"
#include "stack.hpp"

namespace {

using nix_sym::parse_stack_text;
using nix_sym::read_stack_file;
using nix_sym::stack_record_lookup_address;
using nix_sym::StackRecord;

constexpr const char* kBuildId = "c36966051dcc87a30f7ac34a81287ee60a9e1df1";

bool parse_throws(const std::string& text) {
  try {
    (void)parse_stack_text(text);
  } catch (const std::runtime_error&) {
    return true;
  }
  return false;
}

std::string parse_error(const std::string& text) {
  try {
    (void)parse_stack_text(text);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

void test_module_form() {
  const std::string text = "# comment\n"
                           "/nix/store/abc-demo/bin/demo 0x1187 " +
                           std::string(kBuildId) + "\n" +
                           "0x1079\n"
                           "\n"
                           "/nix/store/abc-demo/bin/demo 0xffff\n";
  const std::vector<StackRecord> records = parse_stack_text(text);
  CHECK_EQ(records.size(), static_cast<size_t>(3));
  CHECK_EQ(records[0].module, "/nix/store/abc-demo/bin/demo");
  CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x1187));
  CHECK_EQ(records[0].build_id, kBuildId);
  CHECK_EQ(records[0].line, static_cast<size_t>(2));
  CHECK(!records[0].runtime_address);
  CHECK_EQ(records[0].load_base, static_cast<std::uint64_t>(0));
  CHECK_EQ(records[1].module, records[0].module);
  CHECK_EQ(records[1].address, static_cast<std::uint64_t>(0x1079));
  CHECK_EQ(records[1].build_id, std::string());
  CHECK_EQ(records[1].line, static_cast<size_t>(3));
  CHECK_EQ(records[2].module, records[0].module);
  CHECK_EQ(records[2].address, static_cast<std::uint64_t>(0xffff));
  CHECK_EQ(records[2].line, static_cast<size_t>(5));
}

void test_comments_and_whitespace() {
  const std::vector<StackRecord> records =
      parse_stack_text("  \t\n"
                       "  /nix/store/x/bin/demo   0x10   # trailing comment\r\n"
                       "\r\n"
                       "  0x20# attached comment\r\n");
  CHECK_EQ(records.size(), static_cast<size_t>(2));
  CHECK_EQ(records[0].module, "/nix/store/x/bin/demo");
  CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x10));
  CHECK_EQ(records[1].module, records[0].module);
  CHECK_EQ(records[1].address, static_cast<std::uint64_t>(0x20));

  CHECK(parse_stack_text("").empty());
  CHECK(parse_stack_text("# only a comment\n\n   \n").empty());
  CHECK_EQ(parse_stack_text("/nix/store/x/bin/demo 0x10").size(), static_cast<size_t>(1));
}

void test_runtime_form() {
  {
    const std::vector<StackRecord> records =
        parse_stack_text("/nix/store/x/bin/demo 0x7f001234 @ 0x7f000000\n");
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK(records[0].runtime_address);
    CHECK_EQ(records[0].load_base, static_cast<std::uint64_t>(0x7f000000));
    CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x7f001234));

    std::uint64_t lookup = 0;
    CHECK(stack_record_lookup_address(records[0], &lookup));
    CHECK_EQ(lookup, static_cast<std::uint64_t>(0x1234));
  }
  {
    // Zero load base is valid, as is hexadecimal with upper-case digits.
    const std::vector<StackRecord> records =
        parse_stack_text("/nix/store/x/bin/demo 0x7F001234 @ 0x0\n");
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK(records[0].runtime_address);
    CHECK_EQ(records[0].load_base, static_cast<std::uint64_t>(0));
    CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x7f001234));
  }
  {
    // address == load_base is the module start, not an underflow.
    const std::vector<StackRecord> records =
        parse_stack_text("/nix/store/x/bin/demo 0x200 @ 0x200\n");
    std::uint64_t lookup = 1;
    CHECK(stack_record_lookup_address(records[0], &lookup));
    CHECK_EQ(lookup, static_cast<std::uint64_t>(0));
  }
}

void test_runtime_underflow() {
  CHECK(parse_throws("/nix/store/x/bin/demo 0x100 @ 0x200\n"));
  CHECK(parse_throws(std::string(kBuildId) + " 0x100 @ 0x200\n"));
  CHECK(parse_throws("/nix/store/x/bin/demo 0 @ 0x1\n"));
  const std::string error = parse_error("/nix/store/x/bin/demo 0x100 @ 0x200\n");
  CHECK(error.find("below load base") != std::string::npos);
  CHECK(error.find("line 1") != std::string::npos);
}

void test_build_id_only_form() {
  const std::string text = std::string(kBuildId) + " 0x1187\n" + "0x1079\n" +
                           "0123456789abcdef 0x7f001234 @ 0x7f000000\n";
  const std::vector<StackRecord> records = parse_stack_text(text);
  CHECK_EQ(records.size(), static_cast<size_t>(3));

  CHECK_EQ(records[0].module, std::string());
  CHECK_EQ(records[0].build_id, kBuildId);
  CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x1187));
  CHECK_EQ(records[0].line, static_cast<size_t>(1));
  CHECK(!records[0].runtime_address);

  // A bare address after a build-id record reuses the build id.
  CHECK_EQ(records[1].module, std::string());
  CHECK_EQ(records[1].build_id, kBuildId);
  CHECK_EQ(records[1].address, static_cast<std::uint64_t>(0x1079));

  CHECK(records[2].module.empty());
  CHECK_EQ(records[2].build_id, std::string("0123456789abcdef"));
  CHECK(records[2].runtime_address);
  CHECK_EQ(records[2].load_base, static_cast<std::uint64_t>(0x7f000000));
  CHECK_EQ(records[2].address, static_cast<std::uint64_t>(0x7f001234));

  // Upper-case build ids are valid and preserved.
  const std::vector<StackRecord> upper =
      parse_stack_text("C36966051DCC87A30F7AC34A81287EE60A9E1DF1 0x10\n");
  CHECK_EQ(upper.size(), static_cast<size_t>(1));
  CHECK_EQ(upper[0].build_id, "C36966051DCC87A30F7AC34A81287EE60A9E1DF1");
}

void test_build_id_vs_module() {
  // A first token that is pure hexadecimal with even length >= 8 is a build id.
  for (const std::string build_id :
       {"deadbeef", "deadbeefcafe", "00112233445566778899aabbccddeeff"}) {
    const std::vector<StackRecord> records = parse_stack_text(build_id + " 0x10\n");
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(records[0].module, std::string());
    CHECK_EQ(records[0].build_id, build_id);
    CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x10));
  }
  // Paths and short/odd hexadecimal tokens stay module paths.
  for (const std::string module : {"/nix/store/deadbeef/bin/demo", "./deadbeef", "deadbe",
                                   "deadbee", "deadbeefc", "cafebabe.live"}) {
    const std::vector<StackRecord> records = parse_stack_text(module + " 0x10\n");
    CHECK_EQ(records.size(), static_cast<size_t>(1));
    CHECK_EQ(records[0].module, module);
    CHECK_EQ(records[0].build_id, std::string());
  }
}

void test_invalid_lines() {
  const std::vector<std::string> invalid = {
      "/nix/store/x/bin/demo not-an-address\n",
      "0x1187\n",
      "/nix/store/x/bin/demo 0x10 0x20\n",
      "/nix/store/x/bin/demo 0x10 @\n",
      "/nix/store/x/bin/demo @ 0x10\n",
      "/nix/store/x/bin/demo 0x10 @ not-a-number\n",
      "/nix/store/x/bin/demo 0x10 @@ 0x20\n",
      "/nix/store/x/bin/demo 0x10 0x20 0x30\n",
      "/nix/store/x/bin/demo 0x10 0x20 0x30 0x40\n",
      "/nix/store/x/bin/demo -5\n",
      "/nix/store/x/bin/demo +5\n",
      "/nix/store/x/bin/demo 0x10000000000000000\n",
      std::string(kBuildId) + " 0x10 0x20\n",
      std::string(kBuildId) + " 0x10 @ 0x20 0x30\n",
      "/nix/store/x/bin/demo 0x10 c369\n",
      "/nix/store/x/bin/demo 0x10 c36966051dcc87a30f7ac34a81287ee60a9e1df\n",
  };
  for (const std::string& text : invalid) {
    if (!parse_throws(text)) {
      nix_sym_test::fail(__FILE__, __LINE__, "expected a parse error for: " + text);
    }
  }

  // The error names the one-based line number.
  const std::string error = parse_error("# fine\n/nix/store/x/bin/demo 0x10\nbad\n");
  CHECK(error.find("line 3") != std::string::npos);
}

void test_lookup_address() {
  StackRecord plain;
  plain.address = 0x1234;
  std::uint64_t lookup = 0;
  CHECK(stack_record_lookup_address(plain, &lookup));
  CHECK_EQ(lookup, static_cast<std::uint64_t>(0x1234));

  StackRecord runtime;
  runtime.address = 0x7f001234;
  runtime.load_base = 0x7f000000;
  runtime.runtime_address = true;
  CHECK(stack_record_lookup_address(runtime, &lookup));
  CHECK_EQ(lookup, static_cast<std::uint64_t>(0x1234));

  StackRecord underflow;
  underflow.address = 1;
  underflow.load_base = 2;
  underflow.runtime_address = true;
  lookup = 0xdead;
  CHECK(!stack_record_lookup_address(underflow, &lookup));
  CHECK_EQ(lookup, static_cast<std::uint64_t>(0xdead));
  CHECK(!stack_record_lookup_address(plain, nullptr));
}

void test_read_stack_file() {
  const std::string path = "/tmp/nix-sym-stack-test.txt";
  {
    std::ofstream output(path);
    output << "# comment\n";
    output << "/nix/store/abc-demo/bin/demo 0x1187 " << kBuildId << "\n";
    output << "0x1079\n";
    output << "deadbeef 0x7f001234 @ 0x7f000000\n";
  }
  const std::vector<StackRecord> records = read_stack_file(path);
  CHECK_EQ(records.size(), static_cast<size_t>(3));
  CHECK_EQ(records[0].module, "/nix/store/abc-demo/bin/demo");
  CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x1187));
  CHECK_EQ(records[1].module, records[0].module);
  CHECK_EQ(records[1].address, static_cast<std::uint64_t>(0x1079));
  CHECK_EQ(records[2].build_id, std::string("deadbeef"));
  CHECK(records[2].runtime_address);
  std::remove(path.c_str());

  bool threw = false;
  try {
    (void)read_stack_file("/tmp/nix-sym-stack-does-not-exist.txt");
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()).find("cannot open stack file") != std::string::npos;
  }
  CHECK(threw);
}

} // namespace

int main() {
  test_module_form();
  test_comments_and_whitespace();
  test_runtime_form();
  test_runtime_underflow();
  test_build_id_only_form();
  test_build_id_vs_module();
  test_invalid_lines();
  test_lookup_address();
  test_read_stack_file();
  if (nix_sym_test::failures == 0) {
    std::cout << "stack tests passed\n";
  }
  return nix_sym_test::failures == 0 ? 0 : 1;
}
