#!/usr/bin/env bash
# Produce a real aarch64 (ARM64) Linux ELF ET_CORE on a non-aarch64 host and
# check that nix-sym can read it.
#
# The core comes from a QEMU "virt" guest that boots an Alpine aarch64 kernel
# with a generated initramfs.  The guest init sets core_pattern, dumps a small
# freestanding aarch64 fixture into a kernel core file, gzip+base64 encodes it
# over ttyAMA0, and powers off.  The host decodes the transfer and verifies the
# ELF header (AArch64 / ET_CORE) before optionally running:
#
#   ./build/nix-sym --core .scratch/aarch64/core --json --no-fetch \
#       --no-debuginfod --cache-dir .scratch/aarch64/cache
#
# The fixture is placed in the guest at exactly the same absolute path it has
# on the host, so the NT_FILE entry of the core resolves to a local file and
# libdwfl can use the executable's DWARF and .eh_frame.
#
# Environment:
#   QEMU=/path/to/qemu-system-aarch64  Use this QEMU instead of searching PATH.
#   CLANG=/path/to/clang              Cross compiler (needs the aarch64 target
#                                     and lld as the linker).
#   KERNEL=/path/to/vmlinuz-virt      Skip the kernel download.
#   ROOTFS_TARBALL=/path/to.tar.gz    Skip the Alpine minirootfs download.
#   ALPINE_MIRROR / ALPINE_BRANCH / ALPINE_VERSION
#                                     Override the artifact provenance.
#   QEMU_TIMEOUT=seconds              Guest boot timeout (default 300).
#   NIX_SYM=path                      Symbolizer binary (default build/nix-sym).
#
# Requirements: bash, curl, tar, cpio, gzip, sha256sum, base64, readelf,
# python3, timeout, a clang/lld pair with aarch64 support, and
# qemu-system-aarch64.  Inside the nix-sym repository the usual invocation is:
#
#   nix shell nixpkgs#qemu nixpkgs#clang nixpkgs#lld nixpkgs#cpio \
#       nixpkgs#python3 -c bash scripts/aarch64-core.sh
#
# All generated data stays under .scratch/aarch64/ (gitignored).  No binaries
# are committed.
set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
scratch=${NIX_SYM_AARCH64_SCRATCH:-"$repo_root/.scratch/aarch64"}

# Pinned Alpine provenance.  minirootfs point releases are immutable; the
# netboot kernel is refreshed in place within a stable branch, hence the
# checksum guard below.
alpine_mirror=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
alpine_branch=${ALPINE_BRANCH:-v3.22}
alpine_version=${ALPINE_VERSION:-3.22.8}
kernel_sha256=ebf299aa6829b15166008db24b076ac0a8b3e778c72ed66be488742aaf05d741
minirootfs_sha256=36091efacd083c1cf5a8c837272530bff4fc72db1fbfdef559f52de0d890bca4
qemu_timeout=${QEMU_TIMEOUT:-300}

die() {
  echo "aarch64-core: error: $*" >&2
  exit 1
}

note() {
  echo "aarch64-core: $*" >&2
}

if [[ $scratch == *[[:space:]]* ]]; then
  die "scratch path must not contain whitespace: $scratch (the guest fixture path is passed on the kernel command line)"
fi
mkdir -p "$scratch"
scratch=$(cd -- "$scratch" && pwd)

need() {
  command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"
}
for tool in curl tar cpio gzip sha256sum base64 readelf python3 timeout file; do
  need "$tool"
done

sha256_of() {
  sha256sum "$1" | cut -d' ' -f1
}

fetch_checked() {
  local url=$1 out=$2 want=$3 label=$4 actual
  if [[ -f $out ]]; then
    actual=$(sha256_of "$out")
    if [[ $actual == "$want" ]]; then
      note "$label: reusing $out"
      return 0
    fi
    note "$label: cached copy has an unexpected checksum, downloading again"
    rm -f "$out"
  fi
  note "$label: downloading $url"
  curl -fL --retry 3 --max-time 600 -o "$out.part" "$url" ||
    die "$label: download failed: $url"
  actual=$(sha256_of "$out.part")
  if [[ $actual != "$want" ]]; then
    rm -f "$out.part"
    die "$label: checksum mismatch
  expected: $want
  actual:   $actual
Alpine refreshes netboot artifacts in place within a stable branch.  If the
new kernel is intended, update the pinned checksum in this script; otherwise
pass KERNEL=/path/to/a-cached-vmlinuz-virt (or ROOTFS_TARBALL=...) to reuse a
known-good download."
  fi
  mv "$out.part" "$out"
}

# ---------------------------------------------------------------- QEMU guest

qemu=${QEMU:-}
if [[ -z $qemu ]]; then
  if command -v qemu-system-aarch64 >/dev/null 2>&1; then
    qemu=$(command -v qemu-system-aarch64)
  elif command -v nix >/dev/null 2>&1; then
    note "qemu-system-aarch64 not on PATH, trying nix build nixpkgs#qemu"
    qemu=$(nix build --no-link --print-out-paths nixpkgs#qemu 2>/dev/null || true)/bin/qemu-system-aarch64
  fi
fi
[[ -n $qemu && -x $qemu ]] ||
  die "qemu-system-aarch64 not found; set QEMU=/path/to/qemu-system-aarch64 or run:
  nix shell nixpkgs#qemu nixpkgs#clang nixpkgs#lld nixpkgs#cpio nixpkgs#python3 -c bash scripts/aarch64-core.sh"
note "qemu: $("$qemu" --version | head -n 1)"

# ----------------------------------------------------------- kernel + rootfs

if [[ -n ${KERNEL:-} ]]; then
  vmlinuz=$KERNEL
  [[ -f $vmlinuz ]] || die "KERNEL is not a file: $vmlinuz"
  note "kernel: using KERNEL=$vmlinuz (checksum not verified)"
else
  vmlinuz=$scratch/vmlinuz-virt
  fetch_checked \
    "$alpine_mirror/$alpine_branch/releases/aarch64/netboot/vmlinuz-virt" \
    "$vmlinuz" "$kernel_sha256" "kernel ($alpine_branch netboot)"
fi

if [[ -n ${ROOTFS_TARBALL:-} ]]; then
  rootfs_tar=$ROOTFS_TARBALL
  [[ -f $rootfs_tar ]] || die "ROOTFS_TARBALL is not a file: $rootfs_tar"
  note "minirootfs: using ROOTFS_TARBALL=$rootfs_tar (checksum not verified)"
else
  rootfs_tar=$scratch/alpine-minirootfs-$alpine_version-aarch64.tar.gz
  fetch_checked \
    "$alpine_mirror/$alpine_branch/releases/aarch64/alpine-minirootfs-$alpine_version-aarch64.tar.gz" \
    "$rootfs_tar" "$minirootfs_sha256" "minirootfs $alpine_version"
fi

# Alpine ships vmlinuz-virt as an EFI zboot image ("MZ" + "zimg" header) with a
# gzip-compressed arm64 Image payload.  QEMU can load an uncompressed arm64
# Image directly with -kernel, so unpack it.
image=$scratch/Image
note "kernel: extracting the arm64 Image from $vmlinuz"
python3 - "$vmlinuz" "$image" <<'PY'
import gzip
import io
import struct
import sys

source, destination = sys.argv[1], sys.argv[2]
data = open(source, "rb").read()
if len(data) > 0x40 and data[0x38:0x3c] == b"ARMd":
    raw = data
    kind = "raw arm64 Image"
elif data[:2] == b"MZ" and data[4:8] == b"zimg":
    offset, size = struct.unpack_from("<II", data, 8)
    compression = data[0x18:0x38].split(b"\0", 1)[0]
    if compression != b"gzip":
        sys.exit("unsupported EFI zboot compression: %r" % (compression,))
    raw = gzip.decompress(data[offset:offset + size])
    kind = "EFI zboot, gzip payload at 0x%x" % offset
elif data[:2] == b"\x1f\x8b":
    raw = gzip.decompress(data)
    kind = "gzip Image"
else:
    sys.exit("unrecognised kernel image format (not Image, zboot or gzip)")
if len(raw) < 0x40 or raw[0x38:0x3c] != b"ARMd":
    sys.exit("unpacked kernel does not carry the arm64 Image magic")
open(destination, "wb").write(raw)
print("aarch64-core: kernel: %s -> %d byte arm64 Image" % (kind, len(raw)), file=sys.stderr)
PY

# --------------------------------------------------------------- the fixture

fixture=$scratch/fixture
clang=${CLANG:-clang}
command -v "$clang" >/dev/null 2>&1 ||
  die "clang not found; run inside 'nix shell nixpkgs#clang nixpkgs#lld' or set CLANG=..."
note "fixture: cross-compiling fixtures/aarch64/fixture.c with $clang"
"$clang" \
  --target=aarch64-linux-gnu -march=armv8-a -O0 -g \
  -fno-omit-frame-pointer -funwind-tables -ffreestanding -nostdlib -static \
  -fuse-ld=lld -Wl,-e,_start -Wl,--build-id=sha1 \
  -Wl,-z,max-page-size=4096 \
  -Wl,-T,"$repo_root/fixtures/aarch64/linker.ld" \
  -o "$fixture" "$repo_root/fixtures/aarch64/fixture.c" ||
  die "fixture cross-compilation failed; clang needs the aarch64 target and lld, e.g.
  nix shell nixpkgs#clang nixpkgs#lld -c bash scripts/aarch64-core.sh"
file "$fixture" | grep -q 'ARM aarch64' ||
  die "fixture is not an aarch64 ELF: $(file "$fixture")"

# -------------------------------------------------------------- initramfs

rootfs=$scratch/rootfs
rm -rf "$rootfs"
mkdir -p "$rootfs"
tar -xzf "$rootfs_tar" -C "$rootfs"

# The guest sees the fixture at the same absolute path it has on the host.
# That makes the NT_FILE entry of the core resolvable by nix-sym/libdwfl.
fixture_guest_path=$fixture
mkdir -p "$rootfs$(dirname -- "$fixture_guest_path")"
cp "$fixture" "$rootfs$fixture_guest_path"

# The init runs fixture, lets the kernel write /core, then sends the gzip
# compressed core over the PL011 serial console as base64.  coredump_filter is
# raised to 0x3f so the kernel includes the file-backed private pages (ELF
# header page and .text page); without them libdwfl cannot report the module.
cat > "$rootfs/init" <<'INIT'
#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null
for arg in $(cat /proc/cmdline); do
  case "$arg" in
    fixture=*) FIXTURE=${arg#fixture=} ;;
  esac
done
echo "=== INIT: $(uname -a) ==="
echo "=== INIT: fixture $FIXTURE ==="
echo 0 > /proc/sys/kernel/core_uses_pid 2>/dev/null
echo /core > /proc/sys/kernel/core_pattern
echo 0x3f > /proc/self/coredump_filter
echo "=== INIT: coredump_filter $(cat /proc/self/coredump_filter) ==="
ulimit -c unlimited
echo '=== INIT: running fixture ==='
"$FIXTURE"
echo "=== INIT: fixture returned rc=$? (expected signal) ==="
sleep 1
if [ -f /core ]; then
  echo "=== INIT: core size $(wc -c < /core) ==="
  gzip -9 -c /core > /core.gz
  echo '=== CORE_BEGIN ==='
  base64 /core.gz
  echo '=== CORE_END ==='
else
  echo '=== CORE_MISSING ==='
fi
sync
poweroff -f
INIT
chmod +x "$rootfs/init"

initramfs=$scratch/initramfs.cpio.gz
note "initramfs: packing $rootfs"
(cd "$rootfs" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9) > "$initramfs"

# ------------------------------------------------------------------ qemu run

serial=$scratch/serial.log
rm -f "$serial"
note "boot: qemu-system-aarch64 -M virt -cpu max -m 512M -no-reboot (timeout ${qemu_timeout}s)"
set +e
timeout --signal=TERM "$qemu_timeout" "$qemu" \
  -M virt -cpu max -m 512M -smp 1 \
  -display none -monitor none -serial "file:$serial" \
  -no-reboot -kernel "$image" -initrd "$initramfs" \
  -append "console=ttyAMA0 rdinit=/init panic=1 loglevel=7 fixture=$fixture_guest_path"
qemu_rc=$?
set -e

serial_lf=$scratch/serial.txt
tr -d '\r' < "$serial" > "$serial_lf" 2>/dev/null || true
if ! grep -q '^=== CORE_END ===$' "$serial_lf" ||
  grep -q '^=== CORE_MISSING ===$' "$serial_lf"; then
  echo "--- serial log tail ---" >&2
  tail -n 40 "$serial_lf" >&2
  die "guest did not produce a core (qemu exit $qemu_rc); see $serial"
fi
[[ $qemu_rc -eq 0 || $qemu_rc -eq 124 ]] ||
  die "qemu failed with exit code $qemu_rc; see $serial"

# ----------------------------------------------------------------- host core

core=$scratch/core
rm -f "$core"
note "core: decoding base64+gzip transfer from $serial"
awk '/^=== CORE_BEGIN ===$/{inside=1;next} /^=== CORE_END ===$/{inside=0} inside' \
  "$serial_lf" | base64 -d | gunzip > "$core" ||
  die "failed to decode the transferred core; see $serial"

core_header=$(readelf -h "$core") || die "cannot read the ELF header of $core"
if ! grep -q 'Class:.*ELF64' <<<"$core_header" ||
  ! grep -q 'Type:.*CORE' <<<"$core_header" ||
  ! grep -q 'Machine:.*AArch64' <<<"$core_header"; then
  printf '%s\n' "$core_header" >&2
  die "$core is not an ELF64 AArch64 ET_CORE"
fi
if ! readelf -n "$core" 2>/dev/null | grep -qF -- "$fixture_guest_path"; then
  die "$core has no NT_FILE entry for $fixture_guest_path"
fi

note "core: $(stat -c '%s' "$core") bytes, sha256 $(sha256_of "$core")"
echo "aarch64-core: verified real aarch64 core:"
readelf -h "$core" | grep -E 'Class:|Type:|Machine:'
file "$core"

# ------------------------------------------------------- nix-sym (optional)

sym=${NIX_SYM:-$repo_root/build/nix-sym}
if [[ -x $sym ]]; then
  core_arg=$core
  if [[ $core == "$repo_root"/* ]]; then
    core_arg=${core#"$repo_root"/}
  fi
  cache=$scratch/cache
  rm -rf "$cache"
  note "nix-sym: $sym --core $core_arg --json --no-fetch --no-debuginfod --cache-dir ${cache#"$repo_root"/}"
  set +e
  (cd "$repo_root" && "$sym" --core "$core_arg" --json --no-fetch --no-debuginfod \
    --cache-dir "${cache#"$repo_root"/}" > "$scratch/core.json" 2> "$scratch/core.err")
  sym_rc=$?
  set -e
  if [[ -s $scratch/core.err ]]; then
    cat "$scratch/core.err" >&2
  fi
  [[ $sym_rc -eq 0 || $sym_rc -eq 2 ]] ||
    die "nix-sym failed on $core (exit $sym_rc); see $scratch/core.err"
  python3 - "$scratch/core.json" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, encoding="utf-8") as handle:
    data = json.load(handle)
core = data["core"]
frames = [(thread, frame) for thread in data["threads"] for frame in thread["frames"]]
print("aarch64-core: nix-sym status=%s pid=%s exe=%s signal=%s (%d frame%s, %d thread%s)"
      % (data["status"], core["pid"], core["exe"], core["signal"]["name"],
         len(frames), "" if len(frames) == 1 else "s",
         len(data["threads"]), "" if len(data["threads"]) == 1 else "s"))
for module in data["modules"]:
    print("aarch64-core: module %s status=%s build-id=%s"
          % (module["path"], module["status"], (module["build_id"] or "-")[:16]))
for thread, frame in frames:
    function = frame["function"]["demangled"] or frame["function"]["mangled"] or "?"
    source = frame["source"]
    location = ""
    if source:
        location = " %s:%s" % (source["original"], source["line"])
    print("aarch64-core: frame #%d pc=%s tid=%d %s%s"
          % (frame["index"], frame["pc"], thread["tid"], function, location))
for warning in data["warnings"]:
    print("aarch64-core: warning %s: %s" % (warning["code"], warning["message"]))
if not frames:
    sys.exit("nix-sym produced no frames for the aarch64 core")
PY
  echo "aarch64-core: JSON: $scratch/core.json"
else
  note "build/nix-sym is not built; run 'make -j$(nproc)' and then:"
  note "  ./build/nix-sym --core .scratch/aarch64/core --json --no-fetch --no-debuginfod --cache-dir .scratch/aarch64/cache"
fi

echo "aarch64-core: done: core=$core"
