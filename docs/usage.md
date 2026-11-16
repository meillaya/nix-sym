# Usage

## Build

```sh
nix develop --impure
make -j"$(nproc)"
make check
```

The resulting binary is `build/nix-sym`. The flake also builds it:

```sh
nix build .#nix-sym
./result/bin/nix-sym --version
```

## Inputs

`--core PATH` accepts:

- a plain ELF core file
- a zstd-compressed core, such as the files in `/var/lib/systemd/coredump`
- `-` for a core on stdin

`--from-coredumpctl PID` runs `coredumpctl dump --output` and reads the result.
Use it when the compressed core is owned by `systemd-coredump` and not readable
directly.

```sh
coredumpctl list
nix-sym --from-coredumpctl 12345 --json
```

Exactly one of `--core` and `--from-coredumpctl` is required.

## Backtrace output

```sh
nix-sym --core core.123 --bt
```

`--bt` selects backtrace output. It is the default action when a core is
present. The text output prints one block per thread, the frame index, the
program counter, the demangled function, the module, and the source location.
Inline callers appear under their frame as `inlined by`.

Selection flags:

| Flag | Meaning |
| --- | --- |
| `--thread TID` | Keep one thread. Repeat for several threads. |
| `--max-frames N` | Cap frames per thread. `0` means no cap. Default 256. |
| `--filter REGEX` | Keep frames whose demangled name matches the regular expression. |
| `--no-inline` | Do not print inline frames. |
| `--no-demangle` | Keep the raw symbol names. |
| `--strict` | Exit 2 when the result is partial. |

## JSON

```sh
nix-sym --core core.123 --json
nix-sym --core core.123 --json > backtrace.json
```

`--json` writes only JSON to stdout. Progress and warnings go to stderr. The
schema is `schemas/backtrace-v1.schema.json`. See [json](json.md) for the field
reference.

Useful queries:

```sh
jq '.threads[].frames[] | select(.function.demangled | test("main"))' backtrace.json
jq '[.modules[] | select(.status == "missing-debug") | .path]' backtrace.json
jq '[.warnings[] | .code] | unique' backtrace.json
```

## gdb

```sh
nix-sym --core core.123 --gdb-init nix-sym.gdb
gdb -x nix-sym.gdb
```

The generated file points gdb at the resolved debug outputs and source roots.
It includes the `core-file` command. `--gdb-init -` writes the script to stdout
and cannot be combined with `--json`.

Source extraction is on by default for `--gdb-init`. Use `--no-extract` when
the source archives are large and only a backtrace is needed. The script then
contains comments for mappings that were not materialized.

## Debug resolution and network

By default `nix-sym` resolves debug outputs from the local store and fetches
missing ones from the substituters in `nix config show substituters`:

```sh
nix-sym --core core.123 --json            # may fetch debug outputs
nix-sym --core core.123 --json --no-fetch # local store only
nix-sym --core core.123 --json --offline  # alias for no network
nix-sym --core core.123 --build-missing   # allow nix-store --realise
```

The tool never rebuilds a package unless `--build-missing` is given. If a
substituter does not have the debug output, the module is reported as
`missing-debug` and the rest of the backtrace is still printed.

`nix copy` verifies cache signatures. An unsigned file cache needs a trusted
public key in `trusted-public-keys`, or `require-sigs = false` in the Nix
configuration used for the command. `nix-sym` does not disable signature
checks.

`--debug-dir DIR` adds a debug file directory for modules that are not in the
Nix store. Repeat the flag for several directories.

`--nix-bin PATH` points at the Nix executable. The tool derives the matching
`nix-store` path from it, or falls back to `nix-store` on `PATH`.

## Cache

```sh
nix-sym --core core.123 --json --cache-dir .cache/nix-sym
nix-sym --core core.123 --json --refresh
nix-sym --core core.123 --json --no-cache
```

The default cache is `$XDG_CACHE_HOME/nix-sym`, or `~/.cache/nix-sym` when
`XDG_CACHE_HOME` is unset. A second run with `--no-fetch` and a warm cache does
not call Nix. With fetching enabled, unresolved entries are retried.

The cache keeps:

- debug resolutions by build id
- extracted or merged source trees
- decompressed cores used by gdb

Delete the cache directory to reclaim space. Source trees can be large.

## Modules only

```sh
nix-sym --core core.123 --list-modules
```

This prints one line per module with its status, build id, debug output, and
source path. With `--json`, the modules array is populated and the threads
array is empty.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | A backtrace was produced. Warnings may exist. |
| 1 | Fatal error, such as an unreadable core or an invalid option combination. |
| 2 | `--strict` and at least one frame or module is unresolved. |
| 3 | No frames could be produced. |
| 64 | Usage error. |

## Environment

- `XDG_CACHE_HOME` sets the cache base directory.
- `NO_COLOR` disables colour in text output.
- `PATH` must contain `nix` and `nix-store` unless `--nix-bin` is given.

`nix-sym` inherits the Nix configuration of the invoking user. A daemon-based
store works through the normal Nix commands. A remote or cross-machine core is
limited to store paths that the local Nix can resolve or fetch; see
[limitations](limitations.md).

## Troubleshooting

**`debug-info-missing`**: The package was built without `separateDebugInfo`, the
debug output is not in any configured cache, or the store path was collected.
Check the module status, add a substituter with debug information, or use
`--build-missing` for a local build.

**`fetch-failed`**: `nix copy --from` could not find the debug output. Check
network access and the substituters. The tool reports the Nix error in the
warning.

**Source shows a `/build/...` path only**: The debug output has no `.source`
link. That is expected for packages built before nixpkgs 25.11 or packages
whose `$src` was not available at build time.

**gdb does not show source**: Run `--gdb-init` without `--no-extract`, make
sure the source cache is writable, and check the generated comments. Then run
`gdb -x file` from a directory where the source cache path is valid.

**gdb prints a source file warning**: The source archive is older or newer than
the binary. The line numbers come from the debug information; the warning is
normal when a patched source tree is reconstructed.

**`--from-coredumpctl` finds no entry**: Use the exact PID from
`coredumpctl list`, or dump the core first and pass `--core`.
