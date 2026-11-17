# Changelog

## 0.3.0

- Add `--stack` runtime-address forms (`MODULE RUNTIME @ LOAD_BASE`) and
  build-id-only records resolved through debuginfod.
- Add live thread names from `/proc/<pid>/task/<tid>/comm`, `live-thread-gone`
  handling for exit races, and clearer live attach failures.
- Map `/rustc/<hash>/library/...` automatically from a discovered rust-src
  tree. `RUST_SRC_PATH` and `NIX_SYM_RUST_SRC` are honored; explicit
  `--source-dir` still wins.
- Report missing generated files as `source-generated-file` for directory
  sources.
- Harden core parsing with bounded raw/decompressed size, note bytes, and note
  count. Add an fd-based parser for fuzzing.
- Move archive and overlay extraction into a hardened `archive` component with
  traversal, symlink, device, entry, and size limits.
- Add core, archive, and stack fuzz targets. Add corrupt-core and malformed
  debuginfod response tests.
- Add gdb `file` selection so debuginfod-fetched source can be listed for a
  foreign core.
- Produce and verify a real aarch64 QEMU kernel core, add a reproducible script
  and CI job, and document the cross-architecture path.
- Add a 104-module synthetic core benchmark, a second dogfood pass, and
  performance documentation.
- Add header dependency tracking to the Makefile so class layout changes cannot
  leave stale objects.

## 0.2.0

- Add `--pid` live process attach.
- Add `--stack` module-relative address input for coreless crash reports.
- Add debuginfod fallback by build id, with source fetching and a writable
  client cache. `--debuginfod URL` overrides `DEBUGINFOD_URLS`;
  `--no-debuginfod` disables it.
- Resolve modules in a bounded worker pool instead of one module at a time.
- Add `--source-dir PREFIX=DIR` mappings for Rust standard library paths and
  local source trees.
- Add a fallback to the derivation `src` when an older debug output has no
  `.source` link.
- Correct GHC z-encoding for `$` and `.`; add a real GHC fixture and Haskell
  integration test.
- Add a live-process fixture, stack integration test, source override test,
  debuginfod client tests, ASan and UBSan checks, and libFuzzer targets.
- Add aarch64 evaluation to CI.
- Add the `debuginfod` module field and the `debuginfod` source status to the
  schema documentation.

## 0.1.0

Initial release.

- Read plain and zstd-compressed Linux ELF core dumps.
- Resolve Nix debug outputs from the store and configured substituters.
- Cache debug resolutions by build id and source trees by source identity.
- Unwind threads and frames with libdwfl.
- Print inline frames from DWARF scopes.
- Demangle C++, Rust legacy and v0, and Haskell z-encoded names.
- Reconstruct `/build/...` source paths from `.source` and `.sourceoverlay`.
- Emit text, JSON with schema version 1, and a gdb init file.
- Test fixtures for C++ directory and archive sources, a patched overlay, a
  stripped build, and Rust v0.
