/*
 * Minimal freestanding aarch64 crash fixture for scripts/aarch64-core.sh.
 *
 * It does not use libc: it only needs the raw write syscall and then writes
 * through a NULL pointer to raise SIGSEGV.  The three noinline frames give the
 * core a recognisable call chain (outer_frame -> middle_frame -> crash_here),
 * and the linker script keeps the ELF header page and the text page in
 * separate file-backed mappings so libdwfl does not report the executable
 * twice for the same address range.
 */
typedef unsigned long u64;

static long sys_write(int fd, const char* buf, u64 n) {
  register long x8 asm("x8") = 64; /* __NR_write on aarch64 */
  register long x0 asm("x0") = fd;
  register const char* x1 asm("x1") = buf;
  register u64 x2 asm("x2") = n;
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

__attribute__((noinline)) void crash_here(int depth) {
  volatile u64* p = (volatile u64*)0;
  *p = 0xdead0000UL + (u64)depth;
  for (;;) {
  }
}

__attribute__((noinline)) void middle_frame(int depth) { crash_here(depth + 1); }

__attribute__((noinline)) void outer_frame(void) { middle_frame(0); }

void _start(void) {
  static const char message[] = "fixture: about to crash\n";
  sys_write(1, message, sizeof message - 1);
  outer_frame();
  for (;;) {
  }
}
