#!/usr/bin/env bash
# Build a synthetic core with 100 shared libraries for resolver benchmarks.
#
# Usage: scripts/make-many-modules-core.sh [WORKDIR] [CORE]
# The binary prints a marker at main so gdb can stop after the dynamic loader
# has mapped all 100 libraries, then gcore writes a real ET_CORE.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

work=${1:-.scratch/many-modules}
core=${2:-$work/core}
count=${NIX_SYM_MANY_MODULES:-100}

if ! command -v gcc >/dev/null 2>&1 || ! command -v gdb >/dev/null 2>&1; then
  echo "make-many-modules-core: gcc and gdb are required" >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work/libs"
for i in $(seq 0 $((count - 1))); do
  printf 'int lib%d_fn(void) { return %d; }\n' "$i" "$i" >"$work/libs/lib$i.c"
  gcc -shared -fPIC -O1 -g -o "$work/lib$i.so" "$work/libs/lib$i.c"
done

{
  for i in $(seq 0 $((count - 1))); do
    printf 'extern int lib%d_fn(void);\n' "$i"
  done
  printf 'int main(void) {\n'
  for i in $(seq 0 $((count - 1))); do
    printf '  volatile int v%d = lib%d_fn(); (void)v%d;\n' "$i" "$i" "$i"
  done
  printf '  return 0;\n}\n'
} >"$work/main.c"

libs=()
for i in $(seq 0 $((count - 1))); do
  libs+=("-l$i")
done
gcc -O0 -g -o "$work/main" "$work/main.c" -L"$work" -Wl,-rpath,"$work" "${libs[@]}"

rm -f "$core"
gdb -q -batch \
  -ex 'set pagination off' \
  -ex 'set confirm off' \
  -ex 'break main' \
  -ex run \
  -ex "gcore $core" \
  --args "$work/main" >"$work/gdb.log" 2>&1 || true

if [[ ! -s "$core" ]] || ! file "$core" | grep -q 'core file'; then
  echo "make-many-modules-core: gcore failed; see $work/gdb.log" >&2
  exit 1
fi
echo "make-many-modules-core: wrote $core with $count shared libraries"
