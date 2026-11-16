#include <cstdio>
#include <cstdlib>
#include <string>

volatile int sink = 0;

__attribute__((always_inline)) inline int leaf(int value) {
  volatile int* address = reinterpret_cast<volatile int*>(0x10);
  *address = value;
  return value + 1;
}

__attribute__((noinline)) int crash_here(int value) {
  sink = leaf(value);
  return sink;
}

__attribute__((noinline)) const char* patched_value() {
  return "ORIGINAL";
}

int main() {
  std::printf("fixture value: %s\n", patched_value());
  sink = crash_here(7);
  return 0;
}
