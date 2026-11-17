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
