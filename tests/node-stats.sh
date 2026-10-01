#!/bin/sh
# One node process (seed + volunteer), volunteer limits, local counters and the
# opt-in anonymous statistics. No root, no Docker: warp is built here with its
# store and tracker pointed at throwaway locations, against a mock tracker and
# a local signed repository.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
T="$(mktemp -d)"
TP=18777   # mock tracker
RP=18776   # local repository
NP=18778   # node
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }

for port in $TP $RP $NP; do
    if command -v ss >/dev/null && ss -ltn | grep -q ":$port "; then fail "port $port busy"; fi
done

# ── build a private copy: store under $T, tracker on localhost ──
mkdir -p "$T/build" "$T/store"
cp -r "$ROOT/src" "$ROOT/Makefile" "$T/build/"
# Stale objects would keep the default /var/lib/warp compiled in.
rm -f "$T"/build/src/*.o "$T/build/warp"
( cd "$T/build" && make -s CFLAGS="-O2 -Wall -std=c11 -D_GNU_SOURCE -Wno-unused-const-variable \
    -DWARP_STORE_DIR='\"$T/store\"' -DWARP_TRACKER_URL='\"http://127.0.0.1:$TP/warp\"'" >/dev/null )
W="$T/build/warp"
[ -x "$W" ] || fail "build"
TAG="$("$W" archive-tag)"      # archives are named for the platform the test runs on

# ── mock tracker ──
cat > "$T/tracker.py" <<EOF
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
LOG = "$T/tracker.log"
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header("Content-Type","application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        p = self.path.split("?")[0]
        if   p == "/warp/status":     self.send({"status":"ok","active_peers":5,"total_peers":9,"volunteers":2,"packages":4,"reporting_nodes":3,"total_bytes":5368709120,"total_served":42})
        elif p == "/warp/popularity": self.send({"p1":0,"p2":1,"p3":7,"p4":2})
        elif p == "/warp/peers":      self.send({"peers":[],"count":0})
        else: self.send_response(404); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("Content-Length","0")); body = self.rfile.read(n).decode()
        open(LOG,"a").write(self.path + " " + body + "\n")
        self.send({"status":"ok"})
HTTPServer(("127.0.0.1", $TP), H).serve_forever()
EOF
python3 "$T/tracker.py" & PIDS="$PIDS $!"

# ── local signed repository: p1..p4, ~300 KB each ──
mkdir -p "$T/repo"
for n in 1 2 3 4; do
    d="$T/pk$n"; mkdir -p "$d/files/bin"
    printf '{"name": "p%s", "version": "1.0", "install_bins": ["bin/p%s"]}\n' "$n" "$n" > "$d/manifest.json"
    head -c 300000 /dev/urandom > "$d/files/bin/p$n"
    tar -C "$d" -I 'gzip -n' -cf "$T/repo/p$n-1.0-$TAG.warp" manifest.json files
done
"$W" keygen "$T/priv.hex" "$T/pub.hex" >/dev/null
python3 "$ROOT/tools/make-index.py" "$T/repo" --base-url "http://127.0.0.1:$RP" --key "$T/priv.hex" --warp "$W" >/dev/null
python3 -m http.server "$RP" --bind 127.0.0.1 --directory "$T/repo" >"$T/http.log" 2>&1 & PIDS="$PIDS $!"
sleep 1
"$W" repo disable k1os >/dev/null 2>&1 || true
"$W" repo add lab "http://127.0.0.1:$RP" --pubkey "$(tr -d '\n' < "$T/pub.hex")" >/dev/null
"$W" update >/dev/null || fail "warp update"

cfgval() { python3 -c "import json;print(json.load(open('$T/store/seed.conf'))['$1'])"; }
# python3 is needed anyway; `ss` may be missing on a CI runner
port_open() { python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.3); sys.exit(0 if s.connect_ex(('127.0.0.1', $1)) == 0 else 1)"; }
wait_port() { i=0; while ! port_open "$1"; do i=$((i+1)); [ $i -gt 40 ] && return 1; sleep 0.5; done; }
cached() { ls "$T/store/volunteer/"*.warp 2>/dev/null | wc -l; }

# 1. --help: no node line without a config
if "$W" --help | grep -q "Node:"; then fail "help shows a node line without config"; fi
ok "help has no node line before the first run"

# 2. limits: 10.46G parsed, 2 packages max, least seeded first, well seeded (>=5) skipped
"$W" volunteer --quota 10.46G --packages 2 --port $NP >"$T/node1.log" 2>&1 & NODE=$!; PIDS="$PIDS $NODE"
wait_port $NP || { cat "$T/node1.log"; fail "node did not start"; }
[ "$(cached)" -eq 2 ] || { cat "$T/node1.log"; fail "expected 2 cached packages, got $(cached)"; }
[ "$(cfgval quota_bytes)" = "$(python3 -c 'print(int(10.46*1073741824))')" ] || fail "10.46G not parsed: $(cfgval quota_bytes)"
[ "$(cfgval max_packages)" = "2" ] || fail "max_packages"
[ "$(cfgval volunteer)" = "1" ] || fail "volunteer flag"
curl -s "http://127.0.0.1:$NP/warp/v1/list" | python3 -c '
import json,sys
names=sorted(p["name"] for p in json.load(sys.stdin)["packages"])
assert names==["p1","p2"], names' || fail "cache should hold the two least seeded packages (p1, p2)"
ok "quota 10.46G, 2 packages max, rarest first, well-seeded skipped"

# 3. no consent given → nothing sent to /warp/stats
sleep 1
if [ -f "$T/tracker.log" ] && grep -q "^/warp/stats" "$T/tracker.log"; then fail "stats sent without consent"; fi
ok "no statistics without consent"

# 4. a transfer is counted; a second start is refused politely
sha=$(curl -s "http://127.0.0.1:$NP/warp/v1/list" | python3 -c 'import json,sys;print(json.load(sys.stdin)["packages"][0]["sha256"])')
curl -s -o /dev/null "http://127.0.0.1:$NP/warp/v1/pkg/$sha"
out=$("$W" seed --port $NP 2>&1) || fail "second node start must not fail: $out"
echo "$out" | grep -q "already running" || fail "no 'already running' message: $out"
ok "one node at a time, the second start exits cleanly"

# 5. disable volunteer: SIGHUP reaches the running node, config keeps counters
"$W" volunteer --disable 2>&1 | grep -q "picked up" || fail "running node not notified"
sleep 1
kill -TERM $NODE; wait $NODE 2>/dev/null || true
[ "$(cfgval volunteer)" = "0" ] || fail "volunteer should be off"
[ "$(cfgval served_total)" -ge 1 ] || fail "transfer not counted: served_total=$(cfgval served_total)"
[ "$(cfgval uploaded_total)" -ge 300000 ] || fail "bytes not counted: $(cfgval uploaded_total)"
ok "disable applied live; counters saved on exit (sent $(cfgval uploaded_total) B)"

# 6. unlimited: every package, well-seeded ones too
rm -rf "$T/store/volunteer"
"$W" volunteer --enable --quota all --packages all --port $NP >"$T/node2.log" 2>&1 & NODE=$!; PIDS="$PIDS $NODE"
wait_port $NP || { cat "$T/node2.log"; fail "node 2 did not start"; }
[ "$(cached)" -eq 4 ] || fail "'all' should cache 4 packages, got $(cached)"
ok "--quota all --packages all takes everything"
kill -TERM $NODE; wait $NODE 2>/dev/null || true

# 7. disk reserve: asking to keep more than exists free stops the fill
rm -rf "$T/store/volunteer"
"$W" volunteer --quota all --reserve 900000G --port $NP >"$T/node3.log" 2>&1 & NODE=$!; PIDS="$PIDS $NODE"
wait_port $NP || { cat "$T/node3.log"; fail "node 3 did not start"; }
[ "$(cached)" -eq 0 ] || fail "disk reserve ignored"
grep -q "No room left" "$T/node3.log" || fail "no 'No room' warning"
ok "disk reserve is honoured"
kill -TERM $NODE; wait $NODE 2>/dev/null || true

# 8. consent: default is No (just Enter), then Yes shows exactly what is sent
cat > "$T/ask.py" <<EOF
import os, pty, sys
answer = sys.argv[1]; args = sys.argv[2:]
pid, fd = pty.fork()
if pid == 0:
    os.execv("$W", ["warp"] + args)
out = b""
while True:
    try: chunk = os.read(fd, 4096)
    except OSError: break
    if not chunk: break
    out += chunk
    if b"[y/N]" in chunk: os.write(fd, answer.encode() + b"\r")
os.waitpid(pid, 0)
sys.stdout.write(out.decode(errors="replace"))
EOF
python3 "$T/ask.py" "" stats --consent > "$T/ask1.out"
grep -q '"node_id"' "$T/ask1.out" || fail "disclosure does not show the report"
[ "$(cfgval stats_consent)" = "0" ] || fail "Enter must mean No"
ok "default answer is No; the report was shown first"
python3 "$T/ask.py" "y" stats --consent > "$T/ask2.out"
[ "$(cfgval stats_consent)" = "1" ] || fail "Yes not saved"
nid="$(cfgval node_id)"; [ ${#nid} -eq 32 ] || fail "node_id missing"
ok "Yes is saved, anonymous node id created"

# 9. with consent the node reports; the report has only the documented fields
rm -f "$T/tracker.log"
"$W" volunteer --quota 1G --port $NP >"$T/node4.log" 2>&1 & NODE=$!; PIDS="$PIDS $NODE"
wait_port $NP || { cat "$T/node4.log"; fail "node 4 did not start"; }
sleep 1
grep "^/warp/stats" "$T/tracker.log" | head -1 | sed 's#^/warp/stats ##' | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert set(d)=={"node_id","version","os","arch","volunteer","uploaded_bytes","served","packages"}, sorted(d)
assert d["os"] == "linux" and d["arch"] in ("x86_64","aarch64"), (d["os"], d["arch"])
assert len(d["node_id"])==32' || fail "report fields differ from the disclosure"
ok "report sent with consent, fields match the disclosure"

# 10. stats and help show the network numbers
"$W" stats | grep -q "Nodes online:   5" || fail "network stats missing"
plain() { sed 's/\x1b\[[0-9;]*m//g'; }
"$W" --help | plain | grep -q "network: 5 nodes online" || fail "help line missing network"
"$W" --help | plain | grep -q "Node: running, volunteer" || fail "help line missing node state"
ok "warp stats and warp --help show node and network numbers"

# 11. opt out
"$W" stats --no-stats | grep -q "off" || fail "opt-out message"
[ "$(cfgval stats_consent)" = "0" ] || fail "opt-out not saved"
kill -TERM $NODE; wait $NODE 2>/dev/null || true
ok "opt-out works"

echo "PASS: node-stats"
