#!/usr/bin/env bash
# Build and run the nix-sym libFuzzer targets.
#
# Prefers $CXX when it is clang++ (the only compiler here with libFuzzer
# support), otherwise falls back to clang++. Runs each target for FUZZ_SECONDS
# (default 10) using throwaway corpora seeded from fuzz/corpus, so the
# committed seed files are never modified by the fuzzer.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

fuzz_seconds=${FUZZ_SECONDS:-10}
if ! [[ $fuzz_seconds =~ ^[0-9]+$ ]]; then
  echo "check-fuzz: FUZZ_SECONDS must be a non-negative integer, got: $fuzz_seconds" >&2
  exit 1
fi
if ((fuzz_seconds < 1)); then
  echo "check-fuzz: FUZZ_SECONDS must be at least 1" >&2
  exit 1
fi

# $CXX wins only when it actually is clang++ (or a clang wrapper such as
# ccache clang++); some environments set CXX=g++ for the regular build.
find_fuzzer_cxx() {
  local version
  if [[ -n ${CXX:-} ]] && version=$("$CXX" --version 2>/dev/null) && [[ $version == *clang* ]]; then
    printf '%s\n' "$CXX"
    return 0
  fi
  if command -v clang++ >/dev/null 2>&1; then
    command -v clang++
    return 0
  fi
  return 1
}

if ! cxx=$(find_fuzzer_cxx); then
  echo "check-fuzz: no libFuzzer-capable compiler found." >&2
  echo "check-fuzz: install clang (clang++ must be on PATH), or set CXX to a clang++." >&2
  exit 1
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/nix-sym-fuzz.XXXXXX")
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

# Fail early with a clear message when the compiler has no libFuzzer runtime.
if ! printf 'extern "C" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }\n' |
  "$cxx" -std=c++20 -fsanitize=fuzzer -x c++ - -o "$tmp/fuzzer-probe" >/dev/null 2>&1; then
  echo "check-fuzz: $cxx does not support -fsanitize=fuzzer (libFuzzer is unavailable)" >&2
  exit 1
fi

demangle_bin="$tmp/demangle-fuzz"
json_bin="$tmp/json-fuzz"

run "$cxx" -std=c++20 -O1 -g -fsanitize=fuzzer,address -Isrc \
  fuzz/demangle_fuzz.cpp src/rust_demangle.cpp src/haskell_demangle.cpp src/demangle.cpp \
  -o "$demangle_bin"

run "$cxx" -std=c++20 -O1 -g -fsanitize=fuzzer,address -Isrc \
  fuzz/json_fuzz.cpp src/json.cpp src/model.cpp -o "$json_bin"

# Build throwaway corpora from the committed seeds: one input per line for the
# demangler, and the JSON seed verbatim (it deliberately contains raw control
# characters and UTF-8, which must survive intact).
demangle_corpus="$tmp/corpus-demangle"
json_corpus="$tmp/corpus-json"
artifact_dir="$tmp/artifacts"
mkdir -p "$demangle_corpus" "$json_corpus" "$artifact_dir"

seed_index=0
if [[ -f fuzz/corpus/seed-demangle.txt ]]; then
  while IFS= read -r line || [[ -n $line ]]; do
    [[ -z $line ]] && continue
    printf '%s' "$line" >"$demangle_corpus/seed-$seed_index"
    seed_index=$((seed_index + 1))
  done <fuzz/corpus/seed-demangle.txt
fi
if [[ -f fuzz/corpus/seed-json.txt ]]; then
  cp fuzz/corpus/seed-json.txt "$json_corpus/seed-json"
fi

run "$demangle_bin" -max_total_time="$fuzz_seconds" -rss_limit_mb=2048 \
  -artifact_prefix="$artifact_dir/demangle-" "$demangle_corpus"

run "$json_bin" -max_total_time="$fuzz_seconds" -rss_limit_mb=2048 \
  -artifact_prefix="$artifact_dir/json-" "$json_corpus"

echo "check-fuzz: both fuzz targets completed ${fuzz_seconds}s runs without a crash"
