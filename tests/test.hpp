#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

namespace nix_sym_test {

inline int failures = 0;

inline void fail(const char* file, int line, const std::string& message) {
  std::cerr << file << ":" << line << ": " << message << "\n";
  ++failures;
}

inline void expect(bool condition, const char* expression, const char* file, int line) {
  if (!condition) fail(file, line, std::string("expected ") + expression);
}

template <typename A, typename B>
void expect_eq(const A& actual, const B& expected, const char* expression, const char* file,
               int line) {
  if (!(actual == expected)) {
    std::cerr << file << ":" << line << ": " << expression << "\n"
              << "  actual:   " << actual << "\n"
              << "  expected: " << expected << "\n";
    ++failures;
  }
}

}  // namespace nix_sym_test

#define CHECK(expr) ::nix_sym_test::expect((expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) \
  ::nix_sym_test::expect_eq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)
