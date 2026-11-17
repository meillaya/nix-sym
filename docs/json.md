# JSON output

`nix-sym --json` emits one JSON object on stdout. The format is schema version
1 and is defined by
[`schemas/backtrace-v1.schema.json`](../schemas/backtrace-v1.schema.json).

The output is deterministic: modules are sorted by path, threads by tid,
frames by index, and warnings by code and module.

## Top level

| Field | Type | Meaning |
| --- | --- | --- |
| `schema` | number | Always `1`. |
| `tool` | object | `{"name": "nix-sym", "version": "0.3.0"}`. |
| `status` | string | `complete` or `partial`. |
| `core` | object | Core metadata. |
| `modules` | array | Every module reported by libdwfl. |
| `threads` | array | Threads and frames. Empty with `--list-modules`. |
| `warnings` | array | Structured warnings. |

`status` is `partial` when a warning was recorded or a frame has no symbol.
`--strict` turns a partial result into exit code 2.

## core

| Field | Type | Meaning |
| --- | --- | --- |
| `path` | string | The path passed to `--core`, or `-` for stdin. |
| `compressed` | boolean | The input was zstd-compressed. |
| `pid` | number | Process id from the core. `-1` when unknown. |
| `exe` | string or null | Process name recorded in the core. |
| `signal` | object | `number` and `name` from `NT_SIGINFO`. |
| `truncated` | boolean | Reserved for a future truncation check. |

## modules

| Field | Type | Meaning |
| --- | --- | --- |
| `path` | string | Main file path from the core. |
| `name` | string | Module name from libdwfl. |
| `build_id` | string or null | Hex build id. |
| `status` | string | `resolved`, `missing-debug`, `missing-file`, `no-build-id`, `non-nix`, or `unknown`. |
| `debuginfod` | boolean | The debug info came from a debuginfod server. |
| `drv` | string or null | Derivation path. |
| `debug_output` | string or null | Store path of the debug output. |
| `debug_file` | string or null | The `.debug` file used by libdwfl. |
| `source` | object | Source kind, path, overlay, root, and status. |

`source.kind` is `directory`, `archive`, or `none`. `source.path` is the value
of the `.source` link. `source.overlay` is the `.sourceoverlay` directory.
`source.status` is `available`, `resolved`, `debuginfod`, `debuginfod-failed`, or an error status.

## threads and frames

Each thread has `tid`, `name`, and `frames`. `name` is the `/proc/<pid>/task/<tid>/comm` value for live processes and null when unavailable.

Each frame has:

| Field | Type | Meaning |
| --- | --- | --- |
| `index` | number | Frame index after filtering. |
| `pc` | string | Program counter as a hex string. For a runtime `--stack` record this is the supplied runtime address. |
| `lookup_pc` | string | The adjusted PC used for symbol and line lookup (link-time for runtime stack records). |
| `activation` | boolean | DWARF activation frame flag. |
| `module` | string or null | Module path. |
| `module_name` | string | Module name. |
| `build_id` | string or null | Module build id. |
| `function` | object | `mangled`, `demangled`, `language`. |
| `source` | object or null | Innermost source location. |
| `inline` | array | Inline caller frames. |

`function.language` is `c`, `c++`, `rust`, `haskell`, or `unknown`.

A source location has:

| Field | Type | Meaning |
| --- | --- | --- |
| `original` | string | The DWARF path, with the compilation directory joined. |
| `comp_dir` | string | DWARF compilation directory. |
| `line` | number | Line number. |
| `column` | number | Column number. |
| `resolved` | string or null | Path in the cache or in a source directory when materialized. |
| `status` | string | `resolved`, `available`, `source-unavailable`, `source-missing`, `source-generated-file`, `debuginfod`, `debuginfod-failed`, or `unresolved`. |

Each inline entry has `mangled`, `demangled`, `language`, and `call`, where
`call` is a source location for the call site.

## warnings

Each warning has:

| Field | Type | Meaning |
| --- | --- | --- |
| `code` | string | Stable code. |
| `module` | string or null | Related module or debug path. |
| `message` | string | Human-readable detail. |

Known codes include `no-build-id`, `debug-info-missing`,
`debug-info-mismatch`, `module-file-missing`, `source-unavailable`,
`source-generated-file`, `nix-unavailable`, `fetch-failed`,
`debuginfod-failed`, `live-attach-failed`, `live-thread-gone`,
`stack-build-id-unresolved`, `stack-address-underflow`, `relative-module-path`,
`unsupported-arch`, `truncated-core`, `demangle-failed`, `inline-limit`, and
`invalid-filter`.

## Example

```json
{
  "schema": 1,
  "tool": { "name": "nix-sym", "version": "0.3.0" },
  "status": "partial",
  "core": {
    "path": "/tmp/core.123",
    "compressed": false,
    "pid": 123,
    "exe": "demo",
    "signal": { "number": 11, "name": "SIGSEGV" },
    "truncated": false
  },
  "modules": [
    {
      "path": "/nix/store/...-demo/bin/demo",
      "name": "/nix/store/...-demo/bin/demo",
      "build_id": "c36966051dcc87a30f7ac34a81287ee60a9e1df1",
      "status": "resolved",
      "drv": "/nix/store/...-demo.drv",
      "debug_output": "/nix/store/...-demo-debug",
      "debug_file": "/nix/store/...-demo-debug/lib/debug/.build-id/c3/....debug",
      "source": {
        "kind": "directory",
        "path": "/nix/store/...-demo-cpp",
        "overlay": "/nix/store/...-demo-debug/src/overlay",
        "root": "demo-cpp",
        "status": "available"
      }
    }
  ],
  "threads": [
    {
      "tid": 123,
      "frames": [
        {
          "index": 0,
          "pc": "0x555555555187",
          "lookup_pc": "0x555555555187",
          "activation": true,
          "module": "/nix/store/...-demo/bin/demo",
          "module_name": "/nix/store/...-demo/bin/demo",
          "build_id": "c36966051dcc87a30f7ac34a81287ee60a9e1df1",
          "function": {
            "mangled": "_Z10crash_herei",
            "demangled": "crash_here(int)",
            "language": "c++"
          },
          "source": {
            "original": "/build/demo-cpp/src/main.cpp",
            "comp_dir": "/build/demo-cpp",
            "line": 9,
            "column": 12,
            "resolved": null,
            "status": "available"
          },
          "inline": [
            {
              "mangled": "leaf",
              "demangled": "leaf",
              "language": "c++",
              "call": {
                "original": "/build/demo-cpp/src/main.cpp",
                "comp_dir": "/build/demo-cpp",
                "line": 14,
                "column": 0,
                "resolved": null,
                "status": "available"
              }
            }
          ]
        }
      ]
    }
  ],
  "warnings": []
}
```

## Validation

```sh
python3 - <<'PY'
import json
import jsonschema
jsonschema.validate(json.load(open("backtrace.json")),
                    json.load(open("schemas/backtrace-v1.schema.json")))
print("ok")
PY
```

The dev shell provides `jsonschema`. CI validates the fixture output.
