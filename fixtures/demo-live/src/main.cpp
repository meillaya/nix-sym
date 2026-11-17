#include <sys/prctl.h>
#include <unistd.h>

volatile int sink = 0;

__attribute__((noinline)) int inner(int value) {
  for (;;) {
    sink = value++;
    sleep(1);
  }
}

__attribute__((noinline)) int outer(int value) { return inner(value); }

int main() {
  // Allow a same-user tracing tool even when Yama ptrace_scope is 1.
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
  return outer(3);
}
