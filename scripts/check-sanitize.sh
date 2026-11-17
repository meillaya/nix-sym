#!/usr/bin/env bash
# Standalone AddressSanitizer + UndefinedBehaviorSanitizer check for nix-sym.
#
# Builds and runs the shared-source unit tests (all src/*.cpp except main.cpp),
# the Rust v0 demangler tests, and the Haskell demangler tests with ASan and
# UBSan enabled. Works with a system g++ and inside the dev shell; it only
# needs the same pkg-config dependencies as the Makefile.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

cxx=${CXX:-g++}
if ! command -v "$cxx" >/dev/null 2>&1; then
  echo "check-sanitize: C++ compiler not found: $cxx" >&2
  exit 1
fi

if ! command -v pkg-config >/dev/null 2>&1; then
  echo "check-sanitize: pkg-config is required for libdw/libelf/libzstd/libarchive" >&2
  exit 1
fi

pkg_cflags=$(pkg-config --cflags libdw libelf libzstd libarchive libdebuginfod)
pkg_libs=$(pkg-config --libs libdw libelf libzstd libarchive libdebuginfod)
# shellcheck disable=SC2206 # intentional word splitting
pkg_cflags=($pkg_cflags)
# shellcheck disable=SC2206 # intentional word splitting
pkg_libs=($pkg_libs)

san_flags=(-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -pthread -Isrc)

tmp=$(mktemp -d "${TMPDIR:-/tmp}/nix-sym-sanitize.XXXXXX")
cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT

run() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  "$@"
}

# Abort on the first error instead of merely printing a UBSan diagnostic, and
# make the check fail on leaks. Preserve options supplied by the caller.
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"

mapfile -t sources < <(find src -name '*.cpp' ! -name 'main.cpp' | sort)
if [[ ${#sources[@]} -eq 0 ]]; then
  echo "check-sanitize: no sources found under src/" >&2
  exit 1
fi

# The debuginfod wrapper may not exist yet while it is being developed; only
# link the library once its implementation is present.
link_extra=()
if [[ -f src/debuginfod.cpp ]]; then
  link_extra+=(-ldebuginfod)
fi

unit_bin="$tmp/unit-tests"
run "$cxx" "${san_flags[@]}" "${pkg_cflags[@]}" "${sources[@]}" tests/unit_tests.cpp \
  "${pkg_libs[@]}" "${link_extra[@]}" -o "$unit_bin"
run "$unit_bin"

rust_bin="$tmp/rust-demangle-test"
run "$cxx" "${san_flags[@]}" tests/unit/rust_demangle_test.cpp src/rust_demangle.cpp -o "$rust_bin"
run "$rust_bin"

haskell_bin="$tmp/haskell-demangle-test"
run "$cxx" "${san_flags[@]}" tests/unit/haskell_demangle_test.cpp src/haskell_demangle.cpp \
  -o "$haskell_bin"
run "$haskell_bin"

echo "check-sanitize: all sanitizer checks passed"
