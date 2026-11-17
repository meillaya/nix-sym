#!/usr/bin/env bash
# End-to-end tests. Run outside the Nix build sandbox, where gdb and gcore
# are available. Set NIX_SYM_REQUIRE_CORE=1 to fail when no core can be made.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

if [[ ! -x build/nix-sym ]]; then
  make -j"$(nproc)"
fi

nix_sym=${NIX_SYM:-$root/build/nix-sym}
schema=$root/schemas/backtrace-v1.schema.json
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export XDG_CACHE_HOME="$work/xdg-cache"
mkdir -p "$XDG_CACHE_HOME"

failures=0
note() { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; failures=$((failures + 1)); }

fixture() {
  nix build --no-link --print-out-paths ".#$1"
}

core_from() {
  local binary=$1
  local output=$2
  if scripts/make-core.sh "$binary" "$output" >/dev/null 2>&1; then
    return 0
  fi
  if [[ ${NIX_SYM_REQUIRE_CORE:-0} == 1 ]]; then
    fail "cannot make a core from $binary"
    return 1
  fi
  note "SKIP: cannot make a core from $binary"
  return 2
}

validate_json() {
  local json=$1
  python3 - "$schema" "$json" <<'PY'
import json, sys
schema = json.load(open(sys.argv[1]))
data = json.load(open(sys.argv[2]))
try:
    import jsonschema
except ImportError:
    # Lightweight fallback for CI shells that do not carry the Python package.
    assert data["schema"] == 1
    assert data["status"] in ("complete", "partial")
    assert isinstance(data["tool"], dict)
    assert isinstance(data["core"], dict)
    assert isinstance(data["modules"], list)
    assert isinstance(data["threads"], list)
    assert isinstance(data["warnings"], list)
    for module in data["modules"]:
        assert isinstance(module["path"], str)
        assert isinstance(module["debuginfod"], bool)
    for thread in data["threads"]:
        for frame in thread["frames"]:
            assert isinstance(frame["function"]["demangled"], str)
            assert frame["source"] is None or isinstance(frame["source"], dict)
else:
    jsonschema.validate(data, schema)
PY
}

test_cpp() {
  local name=$1
  local package=$2
  local out core json cache_log
  out=$(fixture "$package")
  core=$work/$name.core
  core_from "$out/bin/demo" "$core" || return 0
  json=$work/$name.json
  "$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-$name" >"$json"
  validate_json "$json"
  jq -e '.threads | length > 0' "$json" >/dev/null || fail "$name: no threads"
  jq -e '[.threads[].frames[] | select(.function.demangled | test("crash_here|leaf"))] | length > 0' \
    "$json" >/dev/null || fail "$name: no fixture functions"
  jq -e '[.threads[].frames[] | select(.source.line > 0)] | length > 0' "$json" >/dev/null ||
    fail "$name: no source lines"
  jq -e '[.threads[].frames[].inline[] | select(.mangled | test("leaf"))] | length > 0' "$json" \
    >/dev/null || fail "$name: no inline frame"
  jq -e '[.modules[] | select(.status == "resolved")] | length > 0' "$json" >/dev/null ||
    fail "$name: no resolved debug module"

  local debug_dirs debug_file pc0
  debug_dirs=$(jq -r '[.modules[] | select(.debug_output != null) | .debug_output + "/lib/debug"] | join(":")' "$json")
  if command -v eu-stack >/dev/null 2>&1 && [[ -n "$debug_dirs" ]]; then
    if ! eu-stack -i -s --debuginfo-path "$debug_dirs" --core "$core" 2>/dev/null |
      grep -q 'leaf'; then
      fail "$name: eu-stack differential lost the inline leaf frame"
    fi
  fi
  debug_file=$(jq -r '[.modules[] | select(.status == "resolved") | .debug_file] | first // empty' "$json")
  pc0=$(jq -r '.threads[0].frames[0].lookup_pc' "$json")
  module_start=""
  if command -v eu-stack >/dev/null 2>&1; then
    module_start=$(eu-stack -l --core "$core" 2>/dev/null |
      awk 'NR == 2 { split($1, parts, "-"); print parts[1]; exit }')
  fi
  if command -v addr2line >/dev/null 2>&1 && [[ -n "$debug_file" && -n "$module_start" ]]; then
    relative=$(python3 -c 'import sys; print(hex(int(sys.argv[1], 16) - int(sys.argv[2], 16)))' "$pc0" "$module_start")
    if ! addr2line -f -i -C -e "$debug_file" "$relative" 2>/dev/null | grep -q 'leaf'; then
      fail "$name: addr2line differential did not find leaf"
    fi
  fi

  local gdb_init=$work/$name.gdb
  "$nix_sym" --core "$core" --gdb-init "$gdb_init" --no-fetch --no-debuginfod --cache-dir "$work/cache-$name" >/dev/null
  if ! gdb -q -batch -x "$gdb_init" -ex "list patched_value" 2>&1 | grep -q PATCHED; then
    fail "$name: gdb does not show the patched source"
  fi

  local second=$work/$name.second.json
  "$nix_sym" --core "$core" --json --offline --cache-dir "$work/cache-$name" >"$second"
  validate_json "$second"
  jq -e '.threads | length > 0' "$second" >/dev/null || fail "$name: offline run lost frames"
  note "ok: $name"
}

test_rust() {
  local out core json
  out=$(fixture fixture-demo-rust)
  core=$work/rust.core
  core_from "$out/bin/demo" "$core" || return 0
  json=$work/rust.json
  "$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-rust" >"$json"
  validate_json "$json"
  jq -e '[.threads[].frames[] | select(.function.language == "rust")] | length > 0' "$json" >/dev/null ||
    fail "rust: no rust frames"
  jq -e '[.threads[].frames[].function.demangled | select(test("^_R"))] | length == 0' "$json" \
    >/dev/null || fail "rust: v0 names were not demangled"
  jq -e '[.threads[].frames[] | select(.function.demangled | test("crash_here|leaf"))] | length > 0' \
    "$json" >/dev/null || fail "rust: no fixture functions"
  note "ok: rust"
}

test_nodbg() {
  local out core json
  out=$(fixture fixture-demo-nodbg)
  core=$work/nodbg.core
  core_from "$out/bin/demo" "$core" || return 0
  json=$work/nodbg.json
  "$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-nodbg" >"$json"
  validate_json "$json"
  jq -e '.status == "partial"' "$json" >/dev/null || fail "nodbg: status is not partial"
  jq -e '[.warnings[].code | select(. == "debug-info-missing")] | length > 0' "$json" >/dev/null ||
    fail "nodbg: no debug-info-missing warning"
  note "ok: nodbg"
}

test_cache() {
  local out core log wrapper json
  out=$(fixture fixture-demo-cpp)
  core=$work/cache.core
  core_from "$out/bin/demo" "$core" || return 0
  rm -rf "$work/cache-hit"
  "$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-hit" >/dev/null
  cat >"$work/fail-nix" <<'SH'
#!/bin/sh
echo "$*" >>"$NIX_SYM_NIX_LOG"
exit 1
SH
  chmod +x "$work/fail-nix"
  log=$work/nix.log
  : >"$log"
  json=$work/cache.json
  NIX_SYM_NIX_LOG="$log" "$nix_sym" --core "$core" --json --cache-dir "$work/cache-hit" \
    --nix-bin "$work/fail-nix" --no-fetch --no-debuginfod >"$json"
  validate_json "$json"
  if [[ -s "$log" ]]; then
    fail "cache: second run still invoked nix"
  fi
  note "ok: cache"
}

test_stdin() {
  local out core json
  out=$(fixture fixture-demo-cpp)
  core=$work/stdin.core
  core_from "$out/bin/demo" "$core" || return 0
  json=$work/stdin.json
  cat "$core" | "$nix_sym" --core - --json --no-fetch --no-debuginfod --cache-dir "$work/cache-stdin" >"$json"
  validate_json "$json"
  jq -e '.core.path == "-"' "$json" >/dev/null || fail "stdin: core path is not -"
  note "ok: stdin"
}


test_stack() {
  local out core json module pc start rel
  out=$(fixture fixture-demo-cpp)
  core=$work/stack.core
  core_from "$out/bin/demo" "$core" || return 0
  module=$("$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-stack" |
    jq -r '.threads[0].frames[0].module')
  pc=$("$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-stack" |
    jq -r '.threads[0].frames[0].pc')
  start=$(eu-stack -l --core "$core" 2>/dev/null |
    awk 'NR == 2 { split($1, parts, "-"); print parts[1]; exit }')
  if [[ -z "$module" || -z "$pc" || -z "$start" ]]; then
    fail "stack: cannot derive a module-relative address"
    return
  fi
  rel=$(python3 -c 'import sys; print(hex(int(sys.argv[1], 16) - int(sys.argv[2], 16)))' "$pc" "$start")
  printf '%s %s\n' "$module" "$rel" >"$work/stack.txt"
  json=$work/stack.json
  "$nix_sym" --stack "$work/stack.txt" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-stack" >"$json"
  validate_json "$json"
  jq -e '.threads[0].frames | length > 0' "$json" >/dev/null || fail "stack: no frames"
  jq -e '[.threads[0].frames[] | select(.function.demangled | test("crash_here|leaf"))] | length > 0'     "$json" >/dev/null || fail "stack: no fixture function"
  jq -e '.threads[0].frames[0].source.line > 0' "$json" >/dev/null || fail "stack: no source line"
  note "ok: stack"
}

test_source_map() {
  local out core json override
  out=$(fixture fixture-demo-cpp)
  core=$work/source-map.core
  core_from "$out/bin/demo" "$core" || return 0
  override=$work/override
  mkdir -p "$override/src"
  printf 'overridden source\n' >"$override/src/main.cpp"
  json=$work/source-map.json
  "$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --source-dir "/build/demo-cpp=$override" \
    --cache-dir "$work/cache-source-map" >"$json"
  validate_json "$json"
  jq -e --arg prefix "$override" \
    '[.threads[].frames[].source | select(.resolved != null and (.resolved | startswith($prefix)))] | length > 0' \
    "$json" >/dev/null || fail "source-map: override was not used"
  note "ok: source-map"
}


test_haskell() {
  local out core json
  out=$(fixture fixture-demo-haskell)
  core=$work/haskell.core
  core_from "$out/bin/demo" "$core" || return 0
  json=$work/haskell.json
  "$nix_sym" --core "$core" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-haskell" >"$json"
  validate_json "$json"
  jq -e '.threads[0].frames | length > 0' "$json" >/dev/null || fail "haskell: no frames"
  jq -e '[.threads[].frames[] | select(.function.language == "haskell")] | length > 0' "$json" \
    >/dev/null || fail "haskell: no haskell frames"
  jq -e '[.threads[].frames[].function.demangled | select(test("^Main\\."))] | length > 0' "$json" \
    >/dev/null || fail "haskell: no Main.* names"
  jq -e '[.threads[].frames[] | select(.source.line > 0)] | length > 0' "$json" >/dev/null ||
    fail "haskell: no source line"
  note "ok: haskell"
}

test_live() {
  local out json pid
  out=$(fixture fixture-demo-live)
  "$out/bin/demo" >/dev/null 2>&1 &
  pid=$!
  sleep 0.5
  json=$work/live.json
  "$nix_sym" --pid "$pid" --json --no-fetch --no-debuginfod --cache-dir "$work/cache-live" >"$json" || true
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  validate_json "$json"
  jq -e '.core.pid > 0' "$json" >/dev/null || fail "live: no pid"
  jq -e '[.threads[].frames[] | select(.function.demangled | test("inner|outer|main"))] | length > 0' \
    "$json" >/dev/null || fail "live: no fixture functions"
  jq -e '[.threads[].frames[] | select(.source.line > 0)] | length > 0' "$json" >/dev/null ||
    fail "live: no source line"
  note "ok: live"
}

test_cpp "cpp" "fixture-demo-cpp"
test_cpp "cpp-tar" "fixture-demo-cpp-tar"
test_rust
test_nodbg
test_cache
test_stdin
test_stack
test_live
test_haskell
test_source_map

if ((failures > 0)); then
  echo "$failures integration test(s) failed" >&2
  exit 1
fi
echo "integration tests passed"
