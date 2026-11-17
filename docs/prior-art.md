# Prior art

`nix-sym` is not the first tool to read a Nix core dump. This page records what
exists and what this project adds, so the boundary stays honest.

## eu-stack and elfutils

[`eu-stack`](https://sourceware.org/elfutils/) is part of elfutils. It can
unwind a core, print inline frames, show source lines, and list modules when
given `--debuginfo-path`. It is the closest existing tool.

The difference is workflow, not DWARF reading. `eu-stack` assumes the user
already knows the debug directories. `nix-sym` resolves them from the store and
the derivation, emits a stable JSON document, and generates a gdb init file.
`eu-stack` remains an excellent oracle for tests.

## gdb and coredumpctl

`gdb` can open a core, and `coredumpctl` can dump one or launch gdb. On NixOS
25.11, `services.nixseparatedebuginfod2.enable = true` makes gdb fetch debug
information and source through the debuginfod protocol. That is the best
interactive path for a NixOS user who has the service enabled.

`nix-sym` targets the batch path: no server, a JSON schema, inline frames as
data, source mapping in the output, and a generated gdb file for the case where
the user wants to continue interactively.

## nixseparatedebuginfod2

[`nixseparatedebuginfod2`](https://github.com/symphorien/nixseparatedebuginfod2)
is a debuginfod server for nixpkgs. It uses the `debug` outputs and the
`index-debug-info` metadata that Hydra writes to the binary cache. Nixpkgs
25.11 added the `.source` and `.sourceoverlay` links that make source serving
possible. The project is GPL-3.0; `nix-sym` does not link or copy it.

The tool and the server solve different problems:

| | nixseparatedebuginfod2 | nix-sym |
| --- | --- | --- |
| Shape | Long-running server | One-shot CLI |
| Transport | HTTP debuginfod | Local store, `nix copy`, then debuginfod |
| Primary client | gdb, valgrind | Shell, CI, agents, gdb |
| Output | Debug files and source | Text, JSON, gdb init |
| Offline | Local store mode | `--offline` |
| Source | Served on request | Extracted and mapped |

0.2 adds an optional debuginfod client for cross-machine cores and mirrors
fetched source for gdb. The server remains an external service.

## systemd-coredump

[`systemd-coredump`](https://www.freedesktop.org/software/systemd/man/latest/coredumpctl.html)
captures cores and `coredumpctl` can print a basic stack. Its traces do not
resolve Nix debug outputs from the derivation and do not reconstruct
`/build/...` sources. `nix-sym` uses `coredumpctl dump` as an input helper and
adds those steps.

## profilers

`samply` and `py-spy` sample live processes. They are not core dump readers.
They are useful for performance work; `nix-sym` is for the post-crash case.

## Nixpkgs debug outputs

Nixpkgs 25.11 changed the debug output layout to include the build id,
executable, source, and patch overlay symlinks. This is the feature that makes
source reconstruction practical. The project relies on that layout when it is
present and degrades to symbols and lines when it is not.

## What is not claimed

- `nix-sym` does not replace `eu-stack` for someone who already has the debug
  paths.
- It does not replace a debuginfod server for a team that wants to serve debug
  information over HTTP.
- It is not a profiler, a crash reporter service, or a symbol server.
- It is not a general DWARF library.
