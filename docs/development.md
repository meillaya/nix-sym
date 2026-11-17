# Development

## Toolchain

The project builds with C++20 and a Makefile. The Nix flake pins nixpkgs and
devenv, and the dev shell supplies:

- g++ and make
- pkg-config
- elfutils (libdw, libelf, libdwfl)
- zstd and libarchive
- gdb, eu-stack, and addr2line
- nix, jq, and Python with jsonschema
- clang separately for the libFuzzer targets. CI provides it with `nix shell`;
  local users need a clang++ with libFuzzer on `PATH`.

```sh
nix develop --impure
make -j"$(nproc)"
make check
```

The build links `libdw`, `libelf`, `libzstd`, `libarchive`, and
`libdebuginfod`. There is no CMake and no third-party C++ JSON or CLI library.
The JSON writer and argument parser are local so the binary has a small
dependency set.

## Tests

`make check` runs:

- `build/unit_tests`: JSON, CLI, demangler dispatch, GDB rendering, live gdb
  rendering, source path mapping, and source overrides.
- `build/rust_demangle_test`: Rust legacy and v0 vectors.
- `build/haskell_demangle_test`: GHC z-encoding vectors.
- `build/debuginfod_test`: disabled, cache, install, and source mirror
  behavior without network.
- `build/stack_test`: stack parsing (link-time, runtime, build-id-only) and
  error cases.
- `build/core_reader_test`: malformed, truncated, non-ELF, invalid-zstd, and
  bounded-note core inputs.
- `build/archive_test`: archive/overlay traversal, symlink, device, limit, and
  SourceResolver regression cases.
- `build/source_auto_test`: automatic `/rustc` mapping, explicit map
  precedence, and generated-file status.
- `fuzz/stack_fuzz`, `fuzz/core_fuzz`, and `fuzz/archive_fuzz` cover the
  untrusted parser paths.

Unit tests run inside the Nix build. They do not need a core dump.

## Fixtures

The flake exposes these fixture packages:

| Attribute | What it tests |
| --- | --- |
| `fixture-demo-cpp` | Directory source, patch overlay, inline C++ frames. |
| `fixture-demo-cpp-tar` | Archive source and patch overlay. |
| `fixture-demo-nodbg` | Partial output without a debug output. |
| `fixture-demo-rust` | Rust v0 names, DWARF function fallback, source. |
| `fixture-demo-live` | Live attach, source, and frame selection. |
| `fixture-demo-haskell` | GHC z-encoded names and Haskell source. |

Build one with:

```sh
nix build .#fixture-demo-cpp
./result/bin/demo
```

The demo programs segfault on purpose after calling an inlined leaf function.
The C++ variants change `ORIGINAL` to `PATCHED` in `postPatch`, so the source
overlay can be checked.

## Integration tests

Run:

```sh
scripts/test-integration.sh
```

The script:

1. Builds the fixtures with `nix build --print-out-paths`.
2. Generates core dumps with `scripts/make-core.sh`.
3. Runs `nix-sym` with text and JSON output.
4. Validates JSON against the schema.
5. Checks frames, inline frames, source lines, module resolution, and gdb
   source listing.
6. Tests `--offline`, stdin, caching, the no-debug fixture, live attach with
   thread names, stack input (link-time, runtime, and reuse forms), Haskell
   demangling, source overrides, malformed debuginfod responses, and a
   104-module synthetic core.
7. Runs the aarch64 core test when `.scratch/aarch64/core` exists.

Core generation uses `gcore` first. If ptrace is unavailable, it falls back to
a kernel core or `coredumpctl`. Set `NIX_SYM_REQUIRE_CORE=1` to make a missing
core a failure.

The script writes a temporary `XDG_CACHE_HOME` so `nix copy` does not need to
write under the user's home. It uses the dev shell's Python for `jsonschema`.

## Core generation

`scripts/make-core.sh BINARY OUTPUT`:

1. Runs the binary under gdb, stops at the signal, and calls `gcore`.
2. If that fails, runs the binary with `ulimit -c unlimited` and looks for a
   kernel core.
3. If that fails, tries `coredumpctl dump -1`.
4. Exits nonzero only when all paths fail.

The fixture programs are designed to fault in a known function so the expected
frame and inline chain are stable.

## Adding a demangler

Language detection lives in `src/demangle.cpp`. Add a function in a separate
source file and declare it in `src/demangle.hpp`, then dispatch on the DWARF
language or the symbol prefix. Keep the function total: return `std::nullopt`
for input that is not yours, and never throw from a frame path.

Tests belong in `tests/unit/` as a standalone binary, or in `tests/unit_tests.cpp`
for cross-module behavior.

## Nix

The package is built by `mkNixSym` in `flake.nix`:

- `makeFlags = [ "PREFIX=$(out)" ]`
- `doCheck = true`
- `separateDebugInfo = true`, so a crashed `nix-sym` is itself symbolizable

The dev shell is defined in `devenv.nix` and pinned in `devenv.yaml`. The flake
pins the same nixpkgs revision as hoard to reuse the local store and cache.

## CI

The CI workflow runs:

- `nix flake check`
- `nix eval .#packages.aarch64-linux.nix-sym.name` and the matching check
  derivation path for aarch64 evaluation
- `nix build .#nix-sym`
- `devenv test`
- the integration script with `NIX_SYM_REQUIRE_CORE=1`
- ASan and UBSan via `scripts/check-sanitize.sh`
- libFuzzer over demanglers, JSON, core, archive, and stack parsers via
  `scripts/check-fuzz.sh`
- a QEMU aarch64 core run via `scripts/aarch64-core.sh`
- a format check with `clang-format --dry-run --Werror`

Integration runs outside the Nix build sandbox so gdb, gcore, and ptrace are
available. The CI integration job runs on Ubuntu, where ptrace is permitted.

## Debuginfod testing

The debuginfod client has unit tests that need no network. To exercise the full
path, start a server and force local resolution to fail:

```sh
nix run nixpkgs#nixseparatedebuginfod2 -- \
  --listen-address 127.0.0.1:1949 \
  --substituter local: --substituter https://cache.nixos.org

nix-sym --core foreign.core --json --nix-bin /bin/false \
  --debuginfod http://127.0.0.1:1949 --no-fetch
```

The server logs `/buildid/...` requests. A glibc core shows `debuginfod: true`
on the glibc modules and source frames under `debuginfod-sources`.

For the gdb source path, keep a core with glibc frames and run:

```sh
NIX_SYM_DEBUGINFOD_URL=http://127.0.0.1:1949 \
  scripts/test-debuginfod-gdb.sh path/to/core
```

The script forces Nix resolution to fail, generates a gdb init file, and checks
that gdb lists a fetched glibc source file. It also verifies that at least one
module has `"debuginfod": true`.

## aarch64 testing

`scripts/aarch64-core.sh` fetches a pinned Alpine aarch64 kernel and
minirootfs, cross-compiles `fixtures/aarch64/fixture.c`, boots QEMU, transfers
a real kernel core over the serial console, verifies `AArch64`/`ET_CORE`, and
runs `nix-sym` on it. See [aarch64](aarch64.md). It needs qemu, clang, lld,
cpio, and python3; CI runs it in a separate job.

## Style

- C++20, two-space indent, 100 columns.
- Braces after every `if`.
- No shell interpolation for core-controlled strings.
- Warnings are errors in CI.
- Documentation follows the house style: sentence case headings, active
  voice, no em dash.

Run `make format` before committing. Run the prose checker from
`/home/mei/nixos/tools/check-prose.py` when it is available.
