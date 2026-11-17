# Dogfood

This is the first ten-core pass on the development machine. It ran with the
0.2 code, `--offline`, `--max-frames 64`, and a cold per-run cache. The point
is to find real failure modes, not to produce a benchmark.

```sh
coredumpctl dump PID --output core.PID
nix-sym --core core.PID --json --no-fetch --no-debuginfod --max-frames 64
```

| PID | Program | Kind | Status | Frames | Resolved debug modules | Missing-debug warnings | Frames with source | Time (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 3330595 | hoard-gguf | Nix Rust | partial | 64 | 0 | 4 | 0 | 1227 |
| 3331091 | hoard-gguf | Nix Rust | partial | 64 | 0 | 4 | 0 | 15 |
| 3208026 | gleam | Nix Rust | partial | 37 | 0 | 6 | 0 | 951 |
| 3170795 | gleam | Nix Rust | partial | 27 | 0 | 6 | 0 | 25 |
| 3146609 | chrome | local prebuilt | partial | 9 | 26 | 0 | 2 | 29316 |
| 3146861 | chrome-headless | local prebuilt | partial | 535 | 24 | 0 | 140 | 10705 |
| 3549554 | bun | Nix prebuilt | partial | 12 | 0 | 6 | 0 | 1028 |
| 4050446 | electron | Nix prebuilt | partial | 26 | 0 | 111 | 0 | 8752 |
| 3962213 | adb | local prebuilt | partial | 10 | 7 | 0 | 4 | 1920 |
| 3330085 | hoard-gguf | local debug build | partial | 8 | 3 | 0 | 6 | 1056 |

All ten produced a usable frame list, and none crashed the symbolizer. Every
result is `partial` because this pass did not fetch debug outputs and because
several binaries were built without `separateDebugInfo`.

## What the pass showed

- Nix Rust binaries from the existing store have no `debug` output. `nix-sym`
  still demangles Rust v0 frames from the ELF symbols, but there are no source
  lines. That is the expected degradation.
- Prebuilt Chrome and Electron binaries have exported symbols and, for some
  modules, system debug packages. The symbolizer finds frames but has no
  package source. That is the non-Nix boundary.
- The local Rust build with embedded debug information resolves symbols and
  source without any Nix work.
- Cold time tracks the number of modules and the time spent in Nix queries.
  The 0.2 resolver runs module lookups in a bounded worker pool; the Chrome
  runs are dominated by the large module list and system debug files, not by
  the symbolizer.
- The default frame cap of 64 was useful for the Chrome core, which has a deep
  stack. Use `--max-frames 0` for a complete dump.
- On the C++ fixture, an empty resolution cache takes about 1.5 seconds and a
  warm cache takes about 9 milliseconds. The warm path performs no Nix or
  debuginfod work.

## What this changes

The next priority is not more demanglers. The pass confirmed that missing debug
outputs are the dominant cause of partial results, which is why 0.2 adds:

- debuginfod fallback by build id for cores from another machine
- `--pid` and `--stack` for input outside a local core file
- `--source-dir` for Rust standard library and local source trees
- a bounded module resolver so cold runs do not serialize every lookup

A second pass should rebuild the local Rust programs with `separateDebugInfo`
and run the same table with a debuginfod server enabled.

## Second pass: debug information and new inputs

The second pass reran the hard cases with debug information available, added
separate-debug Rust and C++ fixtures, a 111-module real core, a QEMU aarch64
core, live attach, and a stack file. It used the 0.3 code.

```sh
nix-sym --core core.PID --json --offline --cache-dir cache/PID
nix-sym --core foreign.core --json --no-fetch --nix-bin /bin/false \
  --debuginfod http://127.0.0.1:1970 --cache-dir cache/foreign
```

| Case | Status | Frames | Resolved modules | Debuginfod modules | Frames with source | Time (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| Rust fixture, separate debug | partial | 9 | 1 | 0 | 4 | 864 |
| C++ fixture, separate debug | partial | 5 | 1 | 0 | 2 | 219 |
| hoard local debug build | partial | 8 | 0 | 0 | 6 | 47 |
| hoard store core, debuginfod | partial | 64 | 2 | 2 | 3 | 841 |
| Chrome headless, local prebuilt | partial | 535 | 0 | 0 | 140 | 1458 |
| Electron, Nix prebuilt | partial | 26 | 0 | 0 | 0 | 4481 |
| hoard, Nix separateDebugInfo rebuild | partial | 4 | 1 | 0 | 0 | 58 |
| aarch64 QEMU core | complete | 4 | 0 | 0 | 4 | 12 |
| 104-module synthetic core | partial | 4 | 0 | 0 | 0 | 30-38 |
| live fixture | partial | 7 | 1 | 0 | 1 | 900 |

The hoard store core was forced through debuginfod with `--nix-bin /bin/false`.
Two glibc modules were resolved by build id and three source files were fetched
into the debuginfod source mirror. The generated gdb init loaded the hoard
executable, used the fetched debug directories, and `gdb -ex 'list'` printed
`pthread_kill.c` from the mirror. The reproducibility script is
[`scripts/test-debuginfod-gdb.sh`](../scripts/test-debuginfod-gdb.sh).

The hoard `separateDebugInfo` rebuild came from the current hoard source with
`separateDebugInfo = true` added to its flake package. It proves that a real
Rust Nix package with a debug output resolves through the normal Nix path. The
core was taken at `main` with gdb; it is not a crash reproducer.

The aarch64 core is a real kernel `ET_CORE` from a QEMU TCG guest. See
[aarch64](aarch64.md) for provenance and the reproduction script.

The live fixture reported its thread name (`demo`) and a source line. The Rust
stack file reported `main::crash_here` and, with a discovered rust-src tree,
resolved the `/rustc/<hash>/library/...` path.

## What the second pass showed

- The first pass's partial results were mostly missing debug outputs. When a
  debug output exists, symbols, inline frames, and source work at the same
  level as gdb.
- Debuginfod closes the cross-machine gap for system and Nix modules that the
  local store cannot resolve.
- The aarch64 path works for reading and unwinding a foreign-architecture core
  when the module files are available at the paths recorded in `NT_FILE`.
- Live attach and stack input are no longer toy paths. Both are covered by the
  integration suite and the real-core table above.
- The remaining gaps are source availability (generated and Rust standard
  library paths), not core reading or symbolization.
