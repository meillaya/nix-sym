# Design

`nix-sym` is a small C++20 CLI that connects three existing pieces:

- `libdwfl` reads the core, reports modules and threads, and unwinds frames.
- Nix tells us which `debug` output belongs to a store path.
- The debug output carries the source and patch overlay that nixpkgs 25.11 puts
  beside the build id.

The tool stays deliberately outside the debuginfod server. It is for the batch
case: give it a core, get a resolved backtrace, JSON, or a gdb script.

## Data flow

```
core file (.zst or ELF)
  -> decompress and open
  -> libdwfl core report and attach
  -> module discovery (path, build id)
  -> Nix resolution (derivation, debug output, fetch, cache)
  -> debuginfo_path
  -> thread and frame unwind
  -> symbol, DWARF line, inline scopes
  -> demangler by CU language
  -> /build source mapping, archive extraction, overlay merge
  -> text, JSON, gdb init
```

The order matters. Module discovery and resolution happen before
`dwfl_core_file_attach` so the debug directories are available when libdwfl
loads CFI and DWARF. Live mode attaches with `dwfl_linux_proc_attach` and uses
the same module and symbol pipeline. Stack mode reports each module with
`dwfl_report_elf` at its link-time base and symbolizes the supplied addresses
without unwinding.

## Components

| File | Responsibility |
| --- | --- |
| `src/core_reader.*` | Detect zstd, decompress with bounded limits, open the ELF core, parse core notes, and expose an fd-based parser for fuzzing. |
| `src/archive.*` | Safe archive/overlay extraction with path, symlink, entry, and size limits. |
| `src/dwfl_session.*` | Own the `Dwfl` session, report modules, resolve debug outputs, unwind threads and frames, read inline scopes. |
| `src/symbolizer` work is in `dwfl_session.cpp` | Map addresses to ELF symbols, DWARF lines, inline chains, and language. |
| `src/nix.*` | Resolve a store path and build id to a derivation, debug output, and source links; fetch and cache; batch module requests. |
| `src/debuginfod.*` | Query a debuginfod server by build id for debug info, executables, and source; cache and install in a libdwfl layout. |
| `src/stack.*` | Parse link-time, runtime-with-base, and build-id-only address lists for `--stack`. |
| `src/source.*` | Map DWARF paths, extract archives, merge overlays, cache source trees. |
| `src/demangle.*` | Language detection, C++ demangling with `__cxa_demangle`, dispatch to Rust and Haskell. |
| `src/rust_demangle.cpp` | Rust legacy and v0 symbol parser. |
| `src/haskell_demangle.cpp` | GHC z-encoding decoder. |
| `src/json.*` | Text and schema-versioned JSON renderers. |
| `src/gdb.*` | gdb init generation. |
| `src/process.*` | `fork`/`execve` helper with a timeout and captured output. |

## Core reading

`--core` accepts a plain ELF core, a zstd-compressed core, or `-` for stdin.
`--from-coredumpctl PID` runs `coredumpctl dump --output` first. The reader
checks for the zstd frame magic and streams the decompressed data to a
temporary file. The temporary file lives in the cache, not in `/tmp`, so a
compressed core can be reused for a gdb session.

The core notes provide the signal and process name. The `NT_FILE` mapping is
handled by libdwfl; `NT_PRSTATUS` register sets are used by its unwinder.
Core limits cap raw and decompressed size, per-note bytes, and note count.
`read_core_info_fd` is the fuzz entry point; it never creates a temporary file
for a non-zstd input.

An aarch64 cold core can be read on an x86_64 host because libdwfl uses the
core's ELF class and machine. The module file must exist at the recorded
`NT_FILE` path; there is no `--sysroot` remapping.

## Debug resolution

The module path is the build key. For a path under `/nix/store`:

1. `nix path-info --derivation` finds the `.drv`. This is more reliable than
   `nix-store -q --deriver`, which reports `unknown-deriver` for some
   cache-substituted paths.
2. `nix-store -q --outputs` lists the outputs; the output whose name ends in
   `-debug` is the debug output.
3. If the debug output is not local, `nix copy --from` tries each configured
   substituter. The tool does not rebuild by default. `--build-missing` allows
   `nix-store --realise`.
4. If the derivation is gone, the build id locates the debug file directly at
   `/nix/store/*-debug/lib/debug/.build-id/xx/rest.debug`.
5. If local resolution still fails and a debuginfod URL is configured, the
   client asks by build id. It installs the returned file in
   `<cache>/debuginfod/<buildid>/lib/debug/.build-id/...` and adds that
   directory to the libdwfl path.
6. A cache entry keyed by build id or store path short-circuits the next run.

Modules are resolved in a bounded pool of four workers. Each worker checks the
cache first, so a warm run does not start a Nix process. The substituter list is
loaded lazily and guarded by a mutex.

Candidates are verified by build id, by the `.executable` link, or both. A
debug output for a different build is rejected with a warning instead of being
silently used.

## Frames

`dwfl_frame_pc` gives the frame PC and an activation flag. For symbol and line
lookup, the tool subtracts one from non-activation PCs to land inside the call
instruction, matching the convention used by `addr2line` and `eu-stack`.

Symbol lookup order:

1. `dwfl_module_addrsym` with an address-range check.
2. `dwfl_module_addrname`.
3. The innermost `DW_TAG_subprogram` in the DWARF scopes, preferring
   `DW_AT_linkage_name`.

The third step matters for stripped Rust and C++ binaries, which often keep no
ELF symbol table but do carry DWARF from the debug output.

Inline frames come from `dwfl_module_addrdie` and `dwarf_getscopes`. The tool
walks `DW_TAG_inlined_subroutine` DIEs, reads the abstract origin name, and
reads `DW_AT_call_file` and `DW_AT_call_line` from the concrete DIE. The file
index is resolved through `dwarf_getsrcfiles`.

## Demangling

The compilation unit's `DW_AT_language` selects the demangler. Without DWARF,
the symbol prefix and suffix select a best guess.

- C++ uses `__cxa_demangle`, with compiler suffixes such as `.cold`,
  `.isra.N`, `.constprop.N`, and `.llvm.<hash>` preserved.
- Rust handles legacy `_ZN...17h<hash>E` names and v0 `_R...` names. The v0
  parser covers paths, identifiers, backrefs, basic types, generic arguments,
  and common const data. Unsupported input falls back to the raw name.
- Haskell decodes GHC z-encoding, strips info-table suffixes, and joins module
  and function names with dots. GHC uses `zi` for a module dot in practice; the
  decoder uses context to handle the historical `zi` meaning as well.

Demangling is best-effort. A failed demangle never removes a frame.

## Source mapping

DWARF compilation directories are usually `/build/<sourceRoot>`. For a path
under that directory:

1. Read `.source` and `.sourceoverlay` from the debug output for the module's
   build id.
2. If `.source` is a directory, use it directly when there is no overlay.
   Otherwise merge it into a cache tree.
3. If `.source` is an archive, extract it with libarchive into the cache.
4. Apply overlay files. The overlay contains only files that patches changed.
5. Report both the original path and the resolved path.

Archive extraction rejects absolute paths, `..` components, and escaping
symlinks. The source cache is keyed by source and overlay identity, so a source
tree used by many binaries is extracted once.

If the source is not available and a debuginfod client is active, the source
file is requested by build id. The result is reported as `debuginfod`; for
`--gdb-init` it is mirrored under `debuginfod-sources` and a substitute-path
line is added.

`--source-dir PREFIX=DIR` takes precedence over package source. It covers
Rust standard library paths, local checkouts, and generated trees without
changing the debug output. Without an explicit mapping, a `/rustc/<hash>/`
path is matched to a rust-src tree discovered from `NIX_SYM_RUST_SRC`,
`RUST_SRC_PATH`, the rustc sysroot, or rustup toolchains.

For a directory source that is not materialized, the resolver checks the
candidate file directly. A missing file under `/build/` is reported as
`source-generated-file`, not `source-missing`. Archives are materialized
before file checks.

If the source is not available, the frame keeps its original `/build/...` path
and a status. Generated files and paths outside the source root are unresolved.

## gdb init

The generated file sets:

- `set debug-file-directory` with the resolved debug directories
- `set substitute-path /build/<root> <resolved source root>`
- `file <crashing executable>` selected from the deepest frame
- `core-file <absolute core path>`

It disables debuginfod for the session. The debug directories are colon
separated. The script does not execute any file from the core or the debug
output.

## Cache

The cache root is `$XDG_CACHE_HOME/nix-sym` by default:

```
resolved/<buildid>.txt        Nix and debug resolution
sources/<key>/tree            extracted or merged source
sources/<key>/root.txt        resolved source root
sources/<key>/.complete       completion marker
cores/                        decompressed cores for gdb
```

Writes are atomic. Cache entries are validated against the file system before
use. `--refresh` ignores reads, and `--no-cache` disables reads and writes.
Build-id-only stack records use the debuginfod executable cache; the fetched
path becomes the module path.

## Security

Core files, module names, archives, and debug information are untrusted input.
The tool does not execute a module or a debug file. It runs `nix` and
`nix-store` through `execve` with an argument vector, never a shell, and it
bounds those
calls with a timeout. Archive extraction uses path checks. Commands are built
from store paths and fixed arguments; the binary does not interpolate core
strings into a shell.

Debug information can be large. Resolution only fetches what a module needs,
and cache writes are atomic. Source extraction can consume significant disk
space; use `--no-extract` when only a backtrace is needed.

## Boundaries

The tool does not:

- read kernel core dumps
- support macOS or Windows
- run a debuginfod server
- rebuild a package unless `--build-missing` is explicit
- promise complete Haskell runtime stacks
- support every Rust v0 construct
- guarantee that a debuginfod server has the build id or source path
- unwind a live process when ptrace is blocked
