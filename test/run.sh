#!/usr/bin/env bash
# Host regression test: builds the engine + web panel for macOS/Linux (mock HTTP layer)
# and runs it against a fake file server (redirect, login cookie, Range, dropped connections).
set -euo pipefail
cd "$(dirname "$0")/.."
D=/tmp/turbodl_test; W=localhost:18080
make -s host-test >/dev/null
rm -rf "$D"; mkdir -p "$D/srv" "$D/app/downloads"
python3 -c "import os;f=open('$D/srv/Test File [v1].pkg','wb');f.write(b'\x7fCNT');f.write(os.urandom(300*1024*1024+12345))"
python3 test/server.py "$D/srv" 9555 >/dev/null 2>&1 & SP=$!
(cd "$D" && exec "$OLDPWD/build/host" > host.log 2>&1) & HP=$!
trap 'kill $SP $HP 2>/dev/null || true' EXIT
sleep 1
st() { curl -s $W/api/jobs.tsv | head -1 | awk -F'\t' '{print $2" "$4}'; }
wait_state() { for _ in $(seq 1 90); do [ "$(st | cut -d' ' -f1)" = "$1" ] && return 0; sleep 1; done; return 1; }
pass() { echo "PASS  $*"; }; fail() { echo "FAIL  $*"; exit 1; }

curl -s -d "urls=http://127.0.0.1:9555/download/Test File [v1].pkg" $W/api/add >/dev/null
wait_state 6 && grep -q "add a cookie" <(curl -s $W/api/jobs.tsv) && pass "401 without cookie -> helpful error" || fail "401 handling"
[ "$(curl -s -d domain=127.0.0.1 -d 'cookie=Cookie: logged-in-sig=abc' $W/api/cookie)" = ok ] && pass "cookie saved" || fail "cookie"
curl -s -d id=1 -d op=retry $W/api/job >/dev/null; sleep 4
curl -s -d id=1 -d op=pause $W/api/job >/dev/null; sleep 1.5; p1=$(st | cut -d' ' -f2)
curl -s -d id=1 -d op=resume $W/api/job >/dev/null; sleep 1.5; p2=$(st | cut -d' ' -f2)
[ "$p2" -ge "$p1" ] && [ "$p1" -gt 0 ] && pass "pause/resume keeps progress ($p1 -> $p2)" || fail "resume ($p1 -> $p2)"
kill -9 $HP; sleep .5; (cd "$D" && exec "$OLDPWD/build/host" >> host.log 2>&1) & HP=$!; sleep 1.5
p3=$(st | cut -d' ' -f2); [ "$p3" -ge "$p1" ] && pass "crash + restart resumes ($p3)" || fail "crash resume ($p3)"
wait_state 5 || fail "download didn't finish"
cmp -s "$D/srv/Test File [v1].pkg" "$D/app/downloads/Test File _v1_.pkg" && pass "file is byte-identical" || fail "corrupt file"
grep -q "AddressSanitizer\|runtime error" "$D/host.log" && fail "sanitizer errors (see $D/host.log)" || pass "no ASan/UBSan errors"
