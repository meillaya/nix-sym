# aarch64 core dumps (QEMU system guest)

This document describes the aarch64 runtime evidence for `nix-sym`: a real
aarch64 ELF `ET_CORE` produced by an aarch64 Linux kernel running under
`qemu-system-aarch64` on an x86_64 host, and the `nix-sym` backtrace read from
it on that host.

Everything needed to reproduce it lives in two owned files:

- [`scripts/aarch64-core.sh`](../scripts/aarch64-core.sh): fetches a pinned
  Alpine kernel and minirootfs, cross-compiles the fixture, boots the guest,
  transfers the core out over the serial console, verifies the ELF header and
  runs `nix-sym`.
- [`fixtures/aarch64/`](../fixtures/aarch64): `fixture.c`, a freestanding
  aarch64 program that crashes with `SIGSEGV`, and `linker.ld`, the layout
  that keeps the core readable by libdwfl.

All generated data stays in `.scratch/aarch64/` (gitignored). No binaries are
committed.

## Prerequisites

- Linux host (any architecture QEMU TCG supports; the evidence below is
  x86_64).
- `qemu-system-aarch64`. In the repository dev environment:

  ```sh
  nix shell nixpkgs#qemu nixpkgs#clang nixpkgs#lld nixpkgs#cpio \
    nixpkgs#python3 -c bash scripts/aarch64-core.sh
  ```

  If `nix shell` does not work on the host (for example a read-only Nix
  cache), put `qemu-system-aarch64` on `PATH` or set `QEMU=/path/to/it`.
- `clang` with the aarch64 target and `lld` (`llvm` cross toolchain). Set
  `CLANG=/path/to/clang` to override; a host clang is enough, no aarch64
  cross-GCC is required.
- `curl`, `tar`, `cpio`, `gzip`, `sha256sum`, `base64`, `readelf`, `python3`,
  `timeout`.
- Network access to `dl-cdn.alpinelinux.org`, or cached copies supplied with
  `KERNEL=` and `ROOTFS_TARBALL=`.
- A built `nix-sym` (`make -j"$(nproc)"`). `NIX_SYM=/path/to/nix-sym`
  overrides the default `build/nix-sym`.

## Reproduce

```sh
# 1. Build the symbolizer (skip if build/nix-sym already exists).
make -j"$(nproc)"

# 2. Produce .scratch/aarch64/core and verify it with nix-sym.
nix shell nixpkgs#qemu nixpkgs#clang nixpkgs#lld nixpkgs#cpio \
  nixpkgs#python3 -c bash scripts/aarch64-core.sh

# 3. The exact command the script runs against the produced core.
./build/nix-sym --core .scratch/aarch64/core --json --no-fetch \
  --no-debuginfod --cache-dir .scratch/aarch64/cache
```

The script prints the `readelf -h` proof, the module statuses and the frames.
It leaves these artifacts under `.scratch/aarch64/`:

| Path | Contents |
| --- | --- |
| `core` | The real aarch64 ELF core (`ET_CORE`, machine `AArch64`). |
| `core.json` | `nix-sym --json` output for that core. |
| `core.err` | `nix-sym` stderr (empty in the verified run). |
| `serial.log` | Raw guest console log: kernel boot messages, init output, base64 core transfer. |
| `serial.txt` | `serial.log` with CR characters stripped. |
| `Image` | Uncompressed arm64 kernel Image extracted from the Alpine `vmlinuz-virt`. |
| `vmlinuz-virt`, `alpine-minirootfs-*.tar.gz` | Pinned downloads (checksum guarded). |
| `fixture` | Cross-compiled aarch64 fixture that the guest crashes. |
| `rootfs/`, `initramfs.cpio.gz` | Guest root filesystem and packed initramfs. |
| `cache/` | `nix-sym` cache directory. |

## What the script does

1. **Fetch pinned Alpine artifacts.** Alpine ships an aarch64 kernel and a
   minimal userland that work on the QEMU `virt` machine without any initramfs
   driver modules:

   | Artifact | URL | SHA-256 |
   | --- | --- | --- |
   | minirootfs 3.22.8 | `.../alpine/v3.22/releases/aarch64/alpine-minirootfs-3.22.8-aarch64.tar.gz` | `36091efacd083c1cf5a8c837272530bff4fc72db1fbfdef559f52de0d890bca4` |
   | `vmlinuz-virt` (3.22 netboot) | `.../alpine/v3.22/releases/aarch64/netboot/vmlinuz-virt` | `ebf299aa6829b15166008db24b076ac0a8b3e778c72ed66be488742aaf05d741` |

   The minirootfs URL is immutable (point release in the name). Alpine
   refreshes `netboot/vmlinuz-virt` in place within a stable branch, so the
   script verifies its pinned checksum and fails with an explanatory message
   if upstream moves it. Pass `KERNEL=`/`ROOTFS_TARBALL=` to reuse known-good
   local copies. The verified kernel is

   ```
   Linux version 6.12.115-0-virt (buildozer@build-3-22-aarch64) ... #1-Alpine SMP PREEMPT_DYNAMIC 2026-11-01
   ```

   Its published config (`netboot/config-6.12.115-0-virt`) has
   `CONFIG_SERIAL_AMBA_PL011=y`, `CONFIG_BLK_DEV_INITRD=y`, `CONFIG_RD_GZIP=y`,
   `CONFIG_ELF_CORE=y`, `CONFIG_COREDUMP=y`, so a console initramfs needs no
   out-of-tree modules.

2. **Unpack the kernel.** `vmlinuz-virt` is an EFI `zboot` image
   (`MZ` + `zimg` header) with a gzip-compressed arm64 `Image` payload. QEMU
   `-kernel` wants an uncompressed arm64 Image, so the script decodes
   `payload_offset`/`payload_size` from the zboot header and writes
   `Image` (magic `ARMd` at offset `0x38`).

3. **Cross-compile the fixture.** `fixtures/aarch64/fixture.c` is a
   freestanding program: it writes one line with the raw `write` syscall and
   then stores through a NULL pointer, so the kernel dumps it. Three
   `noinline` frames (`crash_here` ← `middle_frame` ← `outer_frame` ←
   `_start`) make the expected backtrace obvious. It is compiled with clang
   (`--target=aarch64-linux-gnu`), linked with lld as a static non-PIE
   `ET_EXEC`, with a build-id and DWARF.

   `fixtures/aarch64/linker.ld` is load-bearing. It keeps the ELF header page
   and the `.text` page in two different file pages:
   - The first `PT_LOAD` must map file offset 0 at `0x200000` so the ELF
     header itself is present in the core; libdwfl identifies an executable
     module by reading the ELF header from core memory.
   - The `.text` page must map file page 1 (not page 0). Without a custom
     layout, lld places `.text` at an offset congruent with the header page;
     the aarch64 kernel then maps file page 0 at *both* addresses, and libdwfl
     reports two overlapping modules for the executable. Address lookups can
     then land in the symbol-less duplicate and frames lose their symbols.

4. **Build the initramfs.** The script extracts the Alpine minirootfs and
   installs the fixture at exactly the same absolute path it has on the host.
   This matters: the kernel core's `NT_FILE` note stores that path, and
   `nix-sym`/libdwfl opens the module by that path. With the same path valid
   on the host, DWARF and `.eh_frame` resolve. The guest `/init` then:
   - mounts `proc`, `sysfs` and `devtmpfs`,
   - sets `kernel.core_pattern=/core` and clears `core_uses_pid`,
   - raises `/proc/self/coredump_filter` to `0x3f` (see below),
   - runs the fixture, lets the kernel write the core,
   - gzip-compresses `/core` and writes it as base64 between `CORE_BEGIN`/
     `CORE_END` markers on the serial console,
   - powers the machine off.

   `coredump_filter=0x3f` is required because the default `0x33` does not
   include file-backed private pages. In our test with the default filter, the
   kernel emitted the fixture mappings with `FileSiz 0`: the core had
   `NT_PRSTATUS`/`NT_FILE` but no executable bytes. libdwfl then reports only
   `linux-vdso.so.1`; it still unwinds the stack with its architecture-specific
   `ebl` frame-pointer fallback (4 frames in our test), but it cannot produce
   function names or source lines. With `0x3f` the header and text pages are
   in the core, the fixture module is reported, and unwinding uses `.eh_frame`
   CFI with full symbols (see below).

5. **Boot and transfer.** The QEMU command is:

   ```sh
   qemu-system-aarch64 -M virt -cpu max -m 512M -smp 1 \
     -display none -monitor none -serial file:.scratch/aarch64/serial.log \
     -no-reboot -kernel .scratch/aarch64/Image \
     -initrd .scratch/aarch64/initramfs.cpio.gz \
     -append 'console=ttyAMA0 rdinit=/init panic=1 loglevel=7 fixture=<host fixture path>'
   ```

   Serial transfer is used instead of 9p or a virtio disk because the minimal
   Alpine initramfs does not carry those driver modules and the core is tiny
   (163,840 bytes raw, about 5 KiB gzipped). The serial console is a
   `-serial file:` chardev, so the guest is not rate-limited by a UART baud
   rate. Booting the guest takes a few seconds under TCG on this host.

6. **Verify on the host.** The script decodes the markers, then requires
   `readelf -h` to report `ELF64`, `CORE` and `AArch64`, and requires the
   `NT_FILE` note to mention the guest fixture path. It then runs the exact
   `nix-sym` command above and prints a summary, failing if no frames are
   produced.

## Verified result (HEAD 171e478, 2026-11-17)

Host: x86_64 Linux (kernel 7.3.3), QEMU 11.1.2 (TCG, no KVM), clang/lld
23.1.4, elfutils 0.197, `nix-sym` 0.2.0 at commit `171e478`, using the
repository `build/nix-sym`. A pristine `git archive HEAD` build
(`.scratch/aarch64/nix-sym-head`) produced the same backtrace.

ELF proof:

```
$ readelf -h .scratch/aarch64/core
  Class:                             ELF64
  Data:                              2's complement, little endian
  Type:                              CORE (Core file)
  Machine:                           AArch64

$ file .scratch/aarch64/core
core: ELF 64-bit LSB core file, ARM aarch64, version 1 (SYSV), SVR4-style,
      from '/home/mei/Documents/julia/nix-sym/.scratch/aarch64/fixture', ...
      platform: 'aarch64'
```

`nix-sym --json` result: `status: complete`, no warnings, one thread with four
symbolized frames; the executable is reported with its build id and marked
`non-nix` (it is not a store path). Source lines refer to
`fixtures/aarch64/fixture.c`.

| # | PC | Function | Source |
| --- | --- | --- | --- |
| 0 | `0x20101c` | `crash_here` | `fixtures/aarch64/fixture.c:24` |
| 1 | `0x201044` | `middle_frame` | `fixtures/aarch64/fixture.c:29` |
| 2 | `0x201060` | `outer_frame` | `fixtures/aarch64/fixture.c:31` |
| 3 | `0x201088` | `_start` | `fixtures/aarch64/fixture.c:36` |

Core: 163,840 bytes, SIGSEGV, guest kernel 6.12.115-0-virt, `pid`/`tid` vary
per boot. Cross-check with elfutils on the same core shows the unwinder in
use is DWARF CFI from the aarch64 module, not merely a frame-pointer
fallback:

```
$ eu-stack --core=.scratch/aarch64/core -c
#1  0x0000000000201044 - 1 middle_frame  [.../fixture] [eh_frame]
#2  0x0000000000201060 - 1 outer_frame   [.../fixture] [eh_frame]
#3  0x0000000000201088 - 1 _start        [.../fixture] [eh_frame]
```

## Limitations and caveats

- **No aarch64 hardware is involved.** The guest runs under QEMU TCG
  emulation; this is real aarch64 kernel-produced core data, but not evidence
  from physical aarch64 hardware.
- **The fixture is not Nix-built.** Its module status is `non-nix`; the Nix
  store resolution path (derivations, separate debug outputs, store paths as
  core `NT_FILE` entries) is not exercised for aarch64 here. This script is
  item-3 runtime evidence for reading, unwinding and symbolizing a real
  aarch64 core, not an end-to-end Nix resolution test.
- **The module file must exist at the recorded path on the host.**
  `nix-sym` has no `--sysroot`; libdwfl opens the `NT_FILE` path directly. The
  script guarantees this by giving the guest fixture exactly the host's
  absolute path. A core copied from another machine needs its executables
  copied to the same absolute paths (or a wrapper/sysroot feature would be
  required). If the file is missing, frames may still appear via the `ebl`
  fallback but without symbols.
- **Kernel coredump settings matter.** With the kernel default
  `coredump_filter=0x33`, file-backed private pages are not dumped; the core
  then contains no executable bytes and symbolization degrades as described
  above. The script raises the filter to `0x3f`. This is a guest-kernel
  setting, not a `nix-sym` one, but producers of foreign cores should know it.
- **Alpine netboot artifacts are refreshed in place.** The script pins a
  SHA-256 for `vmlinuz-virt`; a refresh within the `v3.22` branch makes the
  script fail with an explicit checksum message. Update the constant or pass
  `KERNEL=`/`ROOTFS_TARBALL=`.
- **Documentation conflict.** [`docs/limitations.md`](limitations.md) currently
  states that a core from a different architecture than the host is not
  supported. This evidence shows the core-read, module-report, CFI-unwind and
  symbolization path works for x86_64-host/aarch64-guest with elfutils 0.197
  and local module files. The claim should be reconciled by whoever owns that
  document; the practical caveat is that module files and language runtime
  environment must be available to the host.
- **Toolchain drift.** A different clang/lld changes the fixture build id and
  the PC values; the script only asserts the ELF type and that frames are
  produced, not exact addresses.
- **Not covered here.** Live attach, debuginfod, separate debug outputs,
  pointer authentication, SVE/SME register decoding, and aarch64 Nix store
  paths are out of scope for this fixture.
