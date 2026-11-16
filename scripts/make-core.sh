#!/usr/bin/env bash
# Build a core dump from a fixture binary.
# Usage: scripts/make-core.sh BINARY OUTPUT
set -euo pipefail

binary=${1:?binary path required}
output=${2:?output path required}

if [[ ! -x "$binary" ]]; then
  echo "make-core: binary is not executable: $binary" >&2
  exit 1
fi

rm -f "$output"

if command -v gdb >/dev/null 2>&1; then
  # gdb stops at the faulting instruction and writes a full core.
  gdb -q -batch \
    -ex "set pagination off" \
    -ex "set confirm off" \
    -ex run \
    -ex "gcore $output" \
    --args "$binary" >/tmp/nix-sym-gdb.log 2>&1 || true
  if [[ -s "$output" ]] && file "$output" | grep -q 'core file'; then
    echo "make-core: wrote $output with gcore"
    exit 0
  fi
fi

# Fallback for hosts where ptrace is blocked: let the kernel or
# systemd-coredump capture the crash.
cd "$(mktemp -d)"
ulimit -c unlimited
set +e
"$binary" >/tmp/nix-sym-crash.log 2>&1
set -e
for candidate in core core.*; do
  if [[ -f "$candidate" ]]; then
    mv "$candidate" "$output"
    echo "make-core: wrote $output with the kernel"
    exit 0
  fi
done

if command -v coredumpctl >/dev/null 2>&1; then
  if coredumpctl dump -1 --output="$output" >/dev/null 2>&1; then
    echo "make-core: wrote $output with coredumpctl"
    exit 0
  fi
fi

echo "make-core: could not generate a core dump (ptrace and coredump are unavailable)" >&2
exit 1
