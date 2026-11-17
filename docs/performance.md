# Performance

These measurements are from a CachyOS x86_64 host, 2026-11, using the 0.3
binary built with `-O2`. They are order-of-magnitude numbers, not a benchmark
suite.

## Resolver warm and cold paths

The C++ fixture has one module with a local debug output.

| Run | Time |
| --- | --- |
| Empty resolution cache | 1533 ms |
| Warm cache | 9 ms |
| Warm cache, repeated | 9 ms |

The cold run spends most of its time in `nix path-info`, `nix-store`, and the
debuginfod client setup. The warm run performs no Nix process and no network
request. The cache is documented in [design](design.md).

## 100-module synthetic core

`scripts/make-many-modules-core.sh` builds a binary linked against 100 shared
libraries, stops at `main` under gdb, and writes a real core.

```sh
scripts/make-many-modules-core.sh .scratch/many-modules
nix-sym --core .scratch/many-modules/core --json --offline --max-frames 0
```

| Metric | Value |
| --- | --- |
| Modules reported | 104 |
| Frames | 4 |
| Threads | 1 |
| Time, empty cache | 30-38 ms |
| Time, repeated | 30-38 ms |

The synthetic libraries are non-Nix and have no build ids, so this measures
module discovery and the bounded resolver, not Nix subprocess latency. It
confirms that module count alone is not a bottleneck.

## Real large core

The Chrome headless core has 64 modules and 535 frames at the default cap of
64 frames. A full run with the default cache took 1.5 s and produced 535
frames and 140 source frames. The Electron core has 111 modules and took 4.5 s.

## Debuginfod

A hoard core forced through a local `nixseparatedebuginfod2` with
`--nix-bin /bin/false` took 841 ms for 64 frames. The first request downloaded
glibc debug info; later runs were faster. The debuginfod cache is under
`$XDG_CACHE_HOME/nix-sym/debuginfod-client`.

## Limits

- The resolver pool is four workers. More workers would mostly increase Nix
  daemon contention.
- Source extraction and archive decompression are bounded but can still take
  seconds for large source trees.
- A cold cache on a core with hundreds of Nix modules is still dominated by
  Nix subprocess calls, not symbolization.
