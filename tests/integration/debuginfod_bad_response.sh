#!/usr/bin/env bash
# Malformed/malicious debuginfod response handling test.
#
# Starts a local fake debuginfod HTTP server (Python http.server, 127.0.0.1
# only) and drives DebuginfodClient::find_debuginfo/find_source through a small
# compiled probe. No real network access and no Nix are required.
#
# Scenarios per build id prefix:
#   a...  404 for every request
#   b...  200 with a Content-Length larger than the body, connection closed
#         mid-response (truncated download)
#   c...  200 with a body that is not a valid debug file nor source file
#   d...  valid debug file / valid source file
#
# The test asserts that malformed responses produce a clean, non-empty error and
# never crash. libdebuginfod (and therefore the DebuginfodClient wrapper) does
# not validate payload contents, so wrong-content responses are accepted as
# clean downloads; that behavior is recorded rather than treated as a failure.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"

cxx=${CXX:-g++}
if ! command -v "$cxx" >/dev/null 2>&1; then
  echo "debuginfod_bad_response: C++ compiler not found: $cxx" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "debuginfod_bad_response: python3 is required for the fake server" >&2
  exit 1
fi
if ! command -v pkg-config >/dev/null 2>&1 ||
  ! pkg-config --exists libdebuginfod; then
  echo "debuginfod_bad_response: libdebuginfod pkg-config metadata is required" >&2
  exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/nix-sym-debuginfod-bad.XXXXXX")
server_pid=""
cleanup() {
  if [[ -n $server_pid ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

checks=0
failures=0
note() { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; failures=$((failures + 1)); }
check() {
  checks=$((checks + 1))
  if ! "$@"; then
    fail "$*"
    return 1
  fi
  return 0
}
check_contains() {
  local file=$1
  local needle=$2
  check grep -qF -- "$needle" "$file"
}

# ---------------------------------------------------------------------------
# Probe: exercises DebuginfodClient through libdebuginfod.
# ---------------------------------------------------------------------------
cat >"$work/probe.cpp" <<'PROBE'
#include "debuginfod.hpp"

#include <cstdio>
#include <string>

namespace {

int usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s MODE URL CACHE_ROOT BUILD_ID [PATH]\n"
               "modes: find-debuginfo, find-source, install-debuginfo\n",
               argv0);
  return 64;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    return usage(argv[0]);
  }
  const std::string mode = argv[1];
  const std::string url = argv[2];
  const std::string cache_root = argv[3];
  const std::string build_id = argv[4];

  nix_sym::DebuginfodClient client(cache_root, {url}, false);
  std::printf("enabled=%d\n", client.enabled() ? 1 : 0);
  if (!client.enabled()) {
    return 2;
  }

  if (mode == "find-debuginfo") {
    const nix_sym::DebuginfodResult result = client.find_debuginfo(build_id);
    std::printf("ok=%d\npath=%s\nerror=%s\n", result.ok ? 1 : 0, result.path.c_str(),
                result.error.c_str());
    return result.ok ? 0 : 2;
  }
  if (mode == "find-source") {
    if (argc < 6) {
      return usage(argv[0]);
    }
    const nix_sym::DebuginfodResult result = client.find_source(build_id, argv[5]);
    std::printf("ok=%d\npath=%s\nerror=%s\n", result.ok ? 1 : 0, result.path.c_str(),
                result.error.c_str());
    return result.ok ? 0 : 2;
  }
  if (mode == "install-debuginfo") {
    if (argc < 6) {
      return usage(argv[0]);
    }
    std::string error;
    const std::string directory = client.install_debuginfo(build_id, argv[5], &error);
    std::printf("ok=%d\ndir=%s\nerror=%s\n", directory.empty() ? 0 : 1, directory.c_str(),
                error.c_str());
    return directory.empty() ? 2 : 0;
  }
  return usage(argv[0]);
}
PROBE

pkg_cflags=$(pkg-config --cflags libdebuginfod)
pkg_libs=$(pkg-config --libs libdebuginfod)
# shellcheck disable=SC2086 # intentional word splitting
if ! "$cxx" -std=c++20 -O1 -g -Isrc $pkg_cflags "$work/probe.cpp" src/debuginfod.cpp \
  $pkg_libs -pthread -o "$work/probe"; then
  echo "debuginfod_bad_response: could not build the probe" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Fake debuginfod server.
# ---------------------------------------------------------------------------
cat >"$work/server.py" <<'SERVER'
import http.server
import socketserver
import sys

port_file = sys.argv[1]
fixture_path = sys.argv[2]
log_path = sys.argv[3]

VALID_SOURCE = b"int valid_source(void) { return 0; }\n"
WRONG_CONTENT = b"this is not an ELF debug file\n"
TRUNCATED_BODY = b"truncated"


def log(message):
    with open(log_path, "a", encoding="utf-8") as log_file:
        log_file.write(message + "\n")


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        # Keep BaseHTTPRequestHandler's stderr noise out of the test output.
        pass

    def _build_id_prefix(self):
        parts = self.path.split("/")
        if len(parts) >= 3 and parts[1] == "buildid":
            return parts[2][:1]
        return ""

    def _serve(self):
        log("REQUEST %s %s" % (self.command, self.path))
        prefix = self._build_id_prefix()
        if prefix == "a":
            self.send_error(404, "not found")
            return
        if prefix == "b":
            # Claim a large body, send a few bytes, close the connection.
            self.send_response(200)
            self.send_header("Content-Length", "65536")
            self.end_headers()
            self.wfile.write(TRUNCATED_BODY)
            self.wfile.flush()
            self.close_connection = True
            try:
                self.connection.close()
            except OSError:
                pass
            return
        if prefix == "c":
            payload = WRONG_CONTENT
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        if prefix == "d":
            if "/source/" in self.path:
                payload = VALID_SOURCE
            elif self.path.endswith("/debuginfo"):
                with open(fixture_path, "rb") as fixture:
                    payload = fixture.read()
            else:
                self.send_error(404, "unknown endpoint")
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        self.send_error(404, "unknown build id")

    def do_GET(self):
        self._serve()

    def do_HEAD(self):
        self._serve()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    httpd = Server(("127.0.0.1", 0), Handler)
    with open(port_file, "w", encoding="utf-8") as handle:
        handle.write(str(httpd.server_address[1]))
    httpd.serve_forever()
SERVER

server_log="$work/server.log"
: >"$server_log"
python3 "$work/server.py" "$work/port" "$work/probe" "$server_log" \
  >"$work/server.out" 2>"$work/server.err" &
server_pid=$!

port=""
for _ in $(seq 1 200); do
  if [[ -s $work/port ]]; then
    port=$(cat "$work/port")
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    break
  fi
  sleep 0.05
done
if [[ -z $port ]]; then
  echo "debuginfod_bad_response: fake server did not start" >&2
  sed -n '1,20p' "$work/server.err" >&2 || true
  exit 1
fi
url="http://127.0.0.1:$port"
note "fake debuginfod server listening on $url"

# Keep libdebuginfod retries bounded; the server is local.
export DEBUGINFOD_TIMEOUT=5

build_404="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
build_truncated="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
build_wrong="cccccccccccccccccccccccccccccccccccccccc"
build_valid="dddddddddddddddddddddddddddddddddddddddd"

# Runs the probe and records stdout/stderr/exit code in globals.
probe_rc=0
probe_out=""
probe_err=""
run_probe() {
  local name=$1
  shift
  probe_rc=0
  "$work/probe" "$@" >"$work/$name.out" 2>"$work/$name.err" || probe_rc=$?
  probe_out="$work/$name.out"
  probe_err="$work/$name.err"
  cat "$probe_out" "$probe_err" >"$work/$name.combined"
  if ((probe_rc >= 128)); then
    fail "$name: probe crashed with exit code $probe_rc"
  fi
}

# ---------------------------------------------------------------------------
# find_debuginfo: 404, truncated body, wrong content, valid file.
# ---------------------------------------------------------------------------
note "scenario: find_debuginfo 404"
run_probe debuginfo-404 find-debuginfo "$url" "$work/cache-404" "$build_404"
check test "$probe_rc" -eq 2
check_contains "$work/debuginfo-404.combined" "failed for build id"
check_contains "$work/debuginfo-404.combined" "debuginfod_find_debuginfo"
check grep -q '^ok=0$' "$probe_out"
check grep -q '^path=$' "$probe_out"

note "scenario: find_debuginfo truncated body"
run_probe debuginfo-truncated find-debuginfo "$url" "$work/cache-truncated" "$build_truncated"
check test "$probe_rc" -eq 2
check_contains "$work/debuginfo-truncated.combined" "debuginfod_find_debuginfo"
check_contains "$work/debuginfo-truncated.combined" "failed"
check grep -q '^ok=0$' "$probe_out"

note "scenario: find_debuginfo wrong content"
run_probe debuginfo-wrong find-debuginfo "$url" "$work/cache-wrong" "$build_wrong"
check test "$probe_rc" -eq 0 -o "$probe_rc" -eq 2
wrong_path=""
if ((probe_rc == 0)); then
  check grep -q '^ok=1$' "$probe_out"
  wrong_path=$(sed -n 's/^path=//p' "$probe_out")
  check test -n "$wrong_path"
  check test -f "$wrong_path"
  note "  libdebuginfod accepted the wrong content without validating it (documented behavior)"
else
  check grep -q '^ok=0$' "$probe_out"
  check test -s "$work/debuginfo-wrong.combined"
  check_contains "$work/debuginfo-wrong.combined" "debuginfod_find_debuginfo"
fi

note "scenario: find_debuginfo valid file"
run_probe debuginfo-valid find-debuginfo "$url" "$work/cache-valid" "$build_valid"
check test "$probe_rc" -eq 0
check grep -q '^ok=1$' "$probe_out"
valid_path=$(sed -n 's/^path=//p' "$probe_out")
check test -n "$valid_path"
check test -f "$valid_path"
if [[ -n $valid_path ]]; then
  magic=$(head -c 4 "$valid_path" | od -An -tx1 | tr -d ' \n')
  check test "$magic" = "7f454c46"
fi

if [[ -n $valid_path ]]; then
  note "scenario: install_debuginfo valid file"
  run_probe install-valid install-debuginfo "$url" "$work/cache-valid" "$build_valid" "$valid_path"
  check test "$probe_rc" -eq 0
  check grep -q '^ok=1$' "$probe_out"
  install_dir=$(sed -n 's/^dir=//p' "$probe_out")
  check test -d "$install_dir"
  installed=$(find "${install_dir:-/nonexistent}" -name '*.debug' -type f | head -n 1)
  check test -n "$installed"
  check cmp -s "$work/probe" "$installed"
fi

# ---------------------------------------------------------------------------
# find_source: 404, truncated body, wrong content, valid source.
# ---------------------------------------------------------------------------
note "scenario: find_source 404"
run_probe source-404 find-source "$url" "$work/cache-source-404" "$build_404" /build/foo/a.c
check test "$probe_rc" -eq 2
check_contains "$work/source-404.combined" "debuginfod_find_source"
check_contains "$work/source-404.combined" "failed for build id"
check grep -q '^ok=0$' "$probe_out"

note "scenario: find_source truncated body"
run_probe source-truncated find-source "$url" "$work/cache-source-truncated" \
  "$build_truncated" /build/foo/a.c
check test "$probe_rc" -eq 2
check_contains "$work/source-truncated.combined" "debuginfod_find_source"
check grep -q '^ok=0$' "$probe_out"

note "scenario: find_source wrong content"
run_probe source-wrong find-source "$url" "$work/cache-source-wrong" "$build_wrong" /build/foo/a.c
check test "$probe_rc" -eq 0 -o "$probe_rc" -eq 2
if ((probe_rc == 0)); then
  wrong_source=$(sed -n 's/^path=//p' "$probe_out")
  check test -f "$wrong_source"
  check grep -qF "this is not an ELF debug file" "$wrong_source"
  note "  libdebuginfod accepted the wrong source content without validating it (documented behavior)"
else
  check test -s "$work/source-wrong.combined"
fi

note "scenario: find_source valid source"
run_probe source-valid find-source "$url" "$work/cache-source-valid" "$build_valid" /build/foo/a.c
check test "$probe_rc" -eq 0
check grep -q '^ok=1$' "$probe_out"
valid_source=$(sed -n 's/^path=//p' "$probe_out")
check test -f "$valid_source"
check grep -q "int valid_source(void) { return 0; }" "$valid_source"

# ---------------------------------------------------------------------------
# Assert the fake server was actually exercised.
# ---------------------------------------------------------------------------
check grep -qF "/buildid/$build_404/debuginfo" "$server_log"
check grep -qF "/buildid/$build_404/source" "$server_log"
check grep -qF "/buildid/$build_truncated/debuginfo" "$server_log"
check grep -qF "/buildid/$build_wrong/debuginfo" "$server_log"
check grep -qF "/buildid/$build_valid/debuginfo" "$server_log"
check grep -qF "/buildid/$build_valid/source" "$server_log"

if ((failures > 0)); then
  printf 'debuginfod_bad_response: %d/%d checks failed\n' "$failures" "$checks" >&2
  if [[ -s $server_log ]]; then
    printf '%s\n' '--- fake server requests ---' >&2
    cat "$server_log" >&2
  fi
  exit 1
fi
echo "debuginfod_bad_response: all $checks checks passed"
