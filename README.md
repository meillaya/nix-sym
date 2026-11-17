# nix-sym

[![CI](https://github.com/meillaya/nix-sym/actions/workflows/ci.yml/badge.svg)](https://github.com/meillaya/nix-sym/actions/workflows/ci.yml)
[![Licence](https://img.shields.io/badge/licence-Apache--2.0-blue.svg)](LICENCE)
[![Release](https://img.shields.io/github/v/release/meillaya/nix-sym)](https://github.com/meillaya/nix-sym/releases)

`nix-sym` turns a Linux core dump from a Nix-built process into a resolved
backtrace without hand-assembling `debug-file-directory` paths.

It loads the modules recorded in the core, finds each module's Nix derivation
and `debug` output, fetches missing debug outputs from the configured
substituters, prints inline frames, demangles C++, Rust, and Haskell names, and
maps DWARF `/build/...` paths back to the source that the debug output carries.
It can also write a gdb init file for interactive work.

```
nix-sym --core core.123 --bt --json
nix-sym --from-coredumpctl 1234 --json
nix-sym --pid 1234 --json
nix-sym --stack addresses.txt --json
nix-sym --core core.123 --gdb-init nix-sym.gdb
```

## Why this exists

Nix store binaries are stripped, debug information lives in a separate output,
and source paths in DWARF point at `/build/...`. NixOS 25.11 already improves
this for interactive gdb: [`nixseparatedebuginfod2`](https://github.com/symphorien/nixseparatedebuginfod2)
serves debug information and source to debuginfod clients, and the debug output
layout now records the build id, the executable, the source, and a patch
overlay. `eu-stack` can also unwind a core when it is given the right debug
paths.

`nix-sym` fills the batch and offline gap:

- One command resolves the debug outputs and prints a result, with no
  debuginfod server and no system configuration.
- Its output has a versioned JSON schema for issue reports, CI artifacts, and
  agents.
- It prints inline frames and demangles C++, Rust v0 and legacy, and Haskell
  z-encoded names.
- It maps `/build/...` paths back to the source archive or directory, including
  files changed by patches.
- It attaches to a live process with `--pid` and symbolizes a coreless
  module-relative address list with `--stack`.
- It can fall back to a debuginfod server for cores from another machine.
- It emits a gdb init file with the right debug directories and source
  substitutions.

It does not replace `nixseparatedebuginfod2`, and it is not a debuginfod
server. See [prior art](docs/prior-art.md) for the boundary.

## Build and run

The project is a C++20 CMake-free Makefile build with a Nix flake and devenv
shell.

```sh
nix develop --impure
make -j"$(nproc)"
make check
```

Run it from the tree:

```sh
./build/nix-sym --core /path/to/core --json
```

Or build the package:

```sh
nix build .#nix-sym
./result/bin/nix-sym --core /path/to/core
```

`nix-sym` needs the `nix` and `nix-store` commands on `PATH` for debug output
resolution. Pass `--nix-bin` if they are elsewhere. It calls Nix as a
subprocess and does not link `libstore` or `libexpr`.

## Read a backtrace

```sh
nix-sym --core core.123 --bt
```

```
Core core.123 (pid 1234) signal SIGSEGV
7 modules, 1 threads, 5 warnings

Thread 1234
  #0  0x555555555187  crash_here(int)
      at /build/demo-cpp/src/main.cpp:9
      inlined by leaf at /build/demo-cpp/src/main.cpp:14
  #1  0x555555555079  main
      at /build/demo-cpp/src/main.cpp:24
```

`--thread TID` selects threads. `--max-frames N` caps the unwind. `--filter
REGEX` keeps frames with a matching demangled name. `--no-inline` and
`--no-demangle` turn off the two enrichment passes.

`--pid PID` attaches to a live process with ptrace. It uses the same module
resolution, source mapping, and JSON output. The process must be dumpable and
the caller must have permission to trace it.

`--stack FILE` reads module-relative addresses for cores or crash reports that
contain no memory image:

```
# MODULE ADDRESS [BUILD-ID]
/nix/store/.../bin/demo 0x1187 c36966051dcc87a30f7ac34a81287ee60a9e1df1
/nix/store/.../bin/demo 0x1079
```

Addresses are link-time addresses. Subtract the load base from a runtime PC
before writing the file. A one-token line reuses the previous module.

A core from `systemd-coredump` is usually compressed. Pass the `.zst` file
directly, pass a decompressed file, or use `coredumpctl`:

```sh
coredumpctl dump 1234 --output core.1234
nix-sym --core core.1234 --json
nix-sym --from-coredumpctl 1234 --json
```

## JSON output

`--json` writes only JSON to stdout. Progress and diagnostics go to stderr.
The schema is [schemas/backtrace-v1.schema.json](schemas/backtrace-v1.schema.json),
and [docs/json.md](docs/json.md) documents the fields.

```sh
nix-sym --core core.1234 --json | jq '.threads[].frames[] | {pc, function: .function.demangled}'
```

The top-level `status` is `complete` or `partial`. A partial result still
contains frames; it records missing debug information in `warnings`. `--strict`
makes a partial result exit 2.

## gdb init file

```sh
nix-sym --core core.1234 --gdb-init nix-sym.gdb
gdb -x nix-sym.gdb
```

The generated file sets the resolved debug directories, source substitutions,
and the core file. It disables debuginfod for that session so gdb does not
prompt or download from a different server. Use `--no-extract` to skip source
extraction; the file then contains comments for source mappings that are not
materialized. Use `--gdb-init -` to write the script to stdout.

Compressed cores are decompressed to the cache before gdb is pointed at them.

## Debug resolution and caching

For each module in the core, `nix-sym`:

1. Finds the store path and build id.
2. Runs `nix path-info --derivation` and `nix-store -q --outputs` to find the
   derivation's `debug` output.
3. Fetches a missing debug output with `nix copy --from` against the
   substituters from `nix config show substituters`.
4. Falls back to a build id search under `/nix/store/*-debug` when the
   derivation is gone.
5. If that fails and a debuginfod URL is configured, asks the server by build
   id. This is the path for a core from another machine. `--debuginfod URL`
   repeats; `DEBUGINFOD_URLS` is used when no flag is given.

Fetches never build by default. Pass `--build-missing` to allow
`nix-store --realise` to build a missing debug output. `--no-fetch` skips Nix
substituter fetches; `--offline` disables both Nix fetches and debuginfod.
`--no-debuginfod` disables the debuginfod environment list.

Debuginfod downloads are cached under `$XDG_CACHE_HOME/nix-sym/debuginfod-client`.
Source files fetched from a server are mirrored under
`debuginfod-sources` for gdb.

Resolutions are cached by build id in `$XDG_CACHE_HOME/nix-sym`. Source
archives and overlays are cached by source and overlay identity. A second run
reuses resolved entries and does not call Nix for them. Missing entries are
retried unless `--no-fetch` is used; with `--no-fetch`, a warm cache avoids Nix
entirely. `nix copy` verifies cache signatures; an unsigned file cache needs a
trusted public key or `require-sigs = false` in the Nix configuration used for
the command.

## Source reconstruction

Nixpkgs 25.11 debug outputs contain, under `lib/debug/.build-id/<id>/`:

- `*.debug`, the debug information
- `*.executable`, a link to the built binary
- `*.source`, a link to `$src`
- `*.sourceoverlay`, a directory with the files that patches changed

`nix-sym` maps `/build/<sourceRoot>` to the source path, extracts archives with
libarchive, and applies the overlay. Directory sources use a hardlink or copy
merge when an overlay exists. Generated files and files from outside the source
tree are reported with an unresolved source status rather than a guessed path.

Debug outputs built before nixpkgs 25.11 have no `.source` links. They still
provide symbols and line numbers, but source reconstruction is unavailable
unless the derivation's `src` can be found.

## Scope and limits

The tool targets Linux cores, live processes, and module-relative address lists
on x86_64 and aarch64. It does not read kernel cores, handle macOS, or promise
GHC runtime stack unwinding. Rust v0 support covers the common subset of the mangling grammar and
falls back to the raw name with a warning when it cannot parse a symbol.

Read [docs/limitations.md](docs/limitations.md) before relying on it for a
live process, a cross-machine core, or a package without a debug output.

## Development

```sh
nix develop --impure
make check
scripts/test-integration.sh
```

The integration script builds the fixture derivations, generates cores with
`gcore`, and checks frames, inline information, source extraction, gdb source
listing, caching, stdin input, live attach, stack input, Haskell demangling,
and source overrides. Set `NIX_SYM_REQUIRE_CORE=1` in CI where a missing core
must fail the job.

`make sanitize` runs the ASan and UBSan suite. `make fuzz` runs the libFuzzer
targets for the demanglers and JSON escaping.

The design is in [docs/design.md](docs/design.md). The first ten-core pass is
in [docs/dogfood.md](docs/dogfood.md). The fixture programs are in
`fixtures/`.

## Licence

Apache-2.0. See [LICENCE](LICENCE).
