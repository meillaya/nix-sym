#include <cstdio>
#include <fstream>
#include <string>

#include "../test.hpp"
#include "stack.hpp"

namespace {

void test_stack_parser() {
  const std::string path = "/tmp/nix-sym-stack-test.txt";
  {
    std::ofstream output(path);
    output << "# comment\n";
    output << "/nix/store/abc-demo/bin/demo 0x1187 c36966051dcc87a30f7ac34a81287ee60a9e1df1\n";
    output << "0x1079\n";
    output << "/nix/store/abc-demo/bin/demo 0xffff\n";
  }
  auto records = nix_sym::read_stack_file(path);
  CHECK_EQ(records.size(), static_cast<size_t>(3));
  CHECK_EQ(records[0].module, "/nix/store/abc-demo/bin/demo");
  CHECK_EQ(records[0].address, static_cast<std::uint64_t>(0x1187));
  CHECK_EQ(records[0].build_id, "c36966051dcc87a30f7ac34a81287ee60a9e1df1");
  CHECK_EQ(records[1].module, records[0].module);
  CHECK_EQ(records[1].address, static_cast<std::uint64_t>(0x1079));
  CHECK_EQ(records[2].address, static_cast<std::uint64_t>(0xffff));
  std::remove(path.c_str());
}

void test_stack_errors() {
  const std::string path = "/tmp/nix-sym-stack-bad.txt";
  {
    std::ofstream output(path);
    output << "/nix/store/abc-demo/bin/demo not-an-address\n";
  }
  bool threw = false;
  try {
    nix_sym::read_stack_file(path);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  std::remove(path.c_str());

  {
    std::ofstream output(path);
    output << "0x1187\n";
  }
  threw = false;
  try {
    nix_sym::read_stack_file(path);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  std::remove(path.c_str());
}

} // namespace

int main() {
  test_stack_parser();
  test_stack_errors();
  if (nix_sym_test::failures == 0) {
    std::cout << "stack tests passed\n";
  }
  return nix_sym_test::failures == 0 ? 0 : 1;
}
