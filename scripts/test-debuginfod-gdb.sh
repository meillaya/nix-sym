#!/usr/bin/env bash
# End-to-end check that debuginfod-fetched debug info and source work in gdb.
#
# Requires NIX_SYM_DEBUGINFOD_URL to point at a debuginfod server that has the
# core's build ids. The core defaults to .scratch/dogfood/3330595.core, a local
# CachyOS core whose glibc modules are absent from the local Nix store.
#
# The script forces Nix resolution to fail (--nix-bin /bin/false), so the only
# way to get debug info is debuginfod. It then generates a gdb init file and
# checks that gdb can list a fetched glibc source file.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

url=${NIX_SYM_DEBUGINFOD_URL:-}
if [[ -z "$url" ]]; then
  echo "test-debuginfod-gdb: set NIX_SYM_DEBUGINFOD_URL to a debuginfod server URL" >&2
  exit 1
fi

core=${1:-.scratch/dogfood/3330595.core}
if [[ ! -s "$core" ]]; then
  echo "test-debuginfod-gdb: core not found: $core" >&2
  echo "Pass a core with glibc frames as the first argument." >&2
  exit 1
fi

if [[ ! -x build/nix-sym ]]; then
  make -j"$(nproc)" build/nix-sym
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export XDG_CACHE_HOME="$work/cache"
mkdir -p "$XDG_CACHE_HOME"

gdb_init=$work/nix-sym.gdb
json=$work/backtrace.json
./build/nix-sym --core "$core" --json --gdb-init "$gdb_init" \
  --nix-bin /bin/false --debuginfod "$url" --no-fetch --extract-sources \
  --cache-dir "$work/nix-sym" --max-frames 16 >"$json" 2>"$work/nix-sym.err"

if ! jq -e '[.modules[] | select(.debuginfod == true)] | length > 0' "$json" >/dev/null; then
  echo "test-debuginfod-gdb: no module was resolved through debuginfod" >&2
  cat "$work/nix-sym.err" >&2
  exit 1
fi

if ! gdb -q -batch -x "$gdb_init" -ex 'frame 0' -ex 'list' 2>"$work/gdb.err" |
  grep -q 'pthread_kill'; then
  echo "test-debuginfod-gdb: gdb did not list a debuginfod source file" >&2
  cat "$work/gdb.err" >&2
  exit 1
fi

echo "test-debuginfod-gdb: debuginfod debug info and source are usable in gdb"
