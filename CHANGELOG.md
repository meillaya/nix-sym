# Changelog

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
