#!/usr/bin/env bash
# Drive a load test: start the server sharing a freshly generated file, sweep N
# concurrent download clients, print throughput, then shut the server down.
#
#   scripts/loadtest.sh <build-dir>
# env knobs: SIZE_MB (16), PORT (5700), CLIENTS (1,2,4,8,16,32,64), REPEATS (2)
set -euo pipefail

BUILD="${1:-build}"
SIZE_MB="${SIZE_MB:-16}"
PORT="${PORT:-5700}"
CLIENTS="${CLIENTS:-1,2,4,8,16,32,64}"
REPEATS="${REPEATS:-2}"

for bin in fileshare_server fileshare_loadtest; do
    if [ ! -x "$BUILD/$bin" ]; then
        echo "missing executable: $BUILD/$bin (build first)" >&2
        exit 1
    fi
done

TMP="$(mktemp -d)"
cleanup() { [ -n "${SRV:-}" ] && kill -9 "$SRV" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

echo "generating ${SIZE_MB} MiB test file..."
head -c "$((SIZE_MB * 1024 * 1024))" /dev/urandom > "$TMP/blob.bin"

mkfifo "$TMP/cmds"
"$BUILD/fileshare_server" --port "$PORT" --config "$TMP/config.json" \
    --add "$TMP/blob.bin=blob" < "$TMP/cmds" > "$TMP/srv.log" 2>&1 &
SRV=$!
exec 3>"$TMP/cmds"
sleep 0.5

"$BUILD/fileshare_loadtest" --host 127.0.0.1 --port "$PORT" --alias blob \
    --clients "$CLIENTS" --repeats "$REPEATS"

echo "shutdown" >&3
exec 3>&-
wait "$SRV" 2>/dev/null || true
SRV=
