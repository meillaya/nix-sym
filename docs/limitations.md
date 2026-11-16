# Limitations

`nix-sym` is a 0.1 tool. The following boundaries are deliberate.

## Platforms

- Linux ELF core dumps only.
- x86_64 and aarch64 are the target architectures. A core from a different
  architecture from the host is not supported.
- Live process attach is not implemented. Use `eu-stack -p`, `gdb -p`, or
  `perf` for a running process.
- Kernel core dumps, vmcore files, and non-ELF crash formats are not supported.
- macOS and Windows are not supported.

## Core files

- Plain ELF and zstd-compressed cores are accepted. Other systemd compression
  choices, such as lz4 or xz, are rejected with a hint to use
  `coredumpctl dump`.
- A core that does not contain `NT_FILE` can produce a partial module list.
- A core whose executable is missing from the store may be reportable only with
  symbols from the debug output, or not at all. The module is marked
  `missing-file`.
- Relative executable paths are resolved against `--cwd` and the current
  directory. A core created with a relative path in a different directory can
  need `--cwd`.

## Debug information

- Debug information must be in a `debug` output, in a configured substituter,
  or in `--debug-dir`.
- `separateDebugInfo` is not enabled for every nixpkgs package. A package
  without a debug output produces frames with ELF symbols only.
- A cross-machine core is limited by what the local Nix can resolve. A store
  path that is not in the local store or a configured cache cannot be mapped
  to its derivation. A debuginfod client fallback is planned, not implemented.
- A collected store path can leave a core with a module that no longer exists.
  The tool continues and reports the module.

## Source reconstruction

- Automatic source reconstruction depends on the nixpkgs 25.11 debug output
  layout: `.source` and `.sourceoverlay` beside the build id. Older debug
  outputs provide symbols and line numbers but not source.
- A source file generated during the build is not part of `$src`. It is
  reported as unresolved rather than guessed.
- Rust standard library paths such as `/rustc/<hash>/library/...` are outside
  the package source and are unresolved. They can be added with a source
  directory if needed.
- Archive extraction supports the formats libarchive can read. Very unusual
  source archives may fail.
- Source trees can be large. The cache is not automatically pruned.
- A directory source with an overlay is merged with hardlinks where possible
  and copies otherwise. The resulting tree uses extra inodes but little data
  when hardlinks work.

## Demangling

- C++ follows the Itanium ABI through `__cxa_demangle`. Very old or vendor
  manglings may not decode.
- Rust legacy and v0 parsers cover the common grammar. A construct outside the
  implemented subset leaves the raw name and adds `demangle-failed`.
- Haskell z-decoding handles GHC info-table names. It does not reconstruct
  higher-level names that were never emitted in the symbol or DWARF.
- GHC runtime stacks are not reconstructed. A Haskell core may stop at the RTS
  or at frames without CFI.
- Swift, Go, D, and other runtimes are not demangled.

## Inline frames

Inline frames depend on DWARF. A binary with line tables but without inline
scopes shows only the outer symbol. The inline chain is capped at 32 entries.
Optimization can move or merge code. A line number is the best mapping the
compiler recorded, not a guarantee about the executing source line.

## Nix behavior

- The tool calls `nix` and `nix-store` as subprocesses. A missing or broken Nix
  installation produces warnings and a backtrace without debug enrichment.
- `nix copy --from` needs a working substituter and enough disk space. A failed
  fetch never blocks the remaining frames.
- `--build-missing` can start a local build. Use it deliberately.
- The cache assumes store paths are immutable. If a store path is replaced by
  unusual means, use `--refresh`.
- The tool does not run inside a Nix build sandbox when it needs to talk to the
  Nix daemon. Run it from a normal user session or a dev shell.

## gdb

- The generated gdb init disables debuginfod for that session. If you rely on
  a debuginfod server, edit the generated file or use gdb directly.
- Paths containing spaces are skipped in the gdb script with a comment. Nix
  store paths do not contain spaces; a custom cache directory might.
- The script points at the cache's decompressed core. Deleting the cache breaks
  a later gdb run.

## Scale

- Cores with hundreds of mapped modules can be slow on a cold cache because
  each Nix store module may require a subprocess call. Warm runs use the cache.
- Very large debug outputs can take time to fetch and disk space to store.
- Symbolizing all threads can be expensive. Use `--thread` and `--max-frames`.
