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

# ---- parallel downloads: limit 2 -> 2 running + 1 queued, all complete intact ----
for n in A B C; do python3 -c "import os;open('$D/srv/par$n.bin','wb').write(os.urandom(120*1024*1024))"; done
curl -s -d parallel=2 -d conns=4 $W/api/settings >/dev/null
curl -s --data-urlencode "urls=http://127.0.0.1:9555/download/parA.bin
http://127.0.0.1:9555/download/parB.bin
http://127.0.0.1:9555/download/parC.bin" -d auto=0 $W/api/add >/dev/null
sleep 2.5
nd=$(curl -s $W/api/jobs.tsv | awk -F'\t' '$7 ~ /^par/ && $2==1' | wc -l | tr -d ' ')
nq=$(curl -s $W/api/jobs.tsv | awk -F'\t' '$7 ~ /^par/ && $2==0' | wc -l | tr -d ' ')
[ "$nd" = 2 ] && [ "$nq" = 1 ] && pass "parallel limit: 2 downloading, 1 queued" || fail "parallel ($nd downloading, $nq queued)"
for _ in $(seq 1 120); do [ "$(curl -s $W/api/jobs.tsv | awk -F'\t' '$7 ~ /^par/ && $2==3' | wc -l | tr -d ' ')" = 3 ] && break; sleep 1; done
ok=1; for n in A B C; do cmp -s "$D/srv/par$n.bin" "$D/app/downloads/par$n.bin" || ok=0; done
[ $ok = 1 ] && pass "3 parallel files byte-identical" || fail "parallel files corrupt/incomplete"
grep -q "AddressSanitizer\|runtime error" "$D/host.log" && fail "sanitizer errors after parallel run" || pass "no sanitizer errors after parallel run"
