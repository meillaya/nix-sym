#include <unistd.h>

volatile int sink = 0;

__attribute__((noinline)) int inner(int value) {
  for (;;) {
    sink = value++;
    sleep(1);
  }
}

__attribute__((noinline)) int outer(int value) { return inner(value); }

int main() { return outer(3); }
