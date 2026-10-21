#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Evidence for the three Megas controller faults. Every check runs against the
# real controller with an isolated HOME and a real qcow2 image, and prints the
# journal line that proves the outcome. No check is allowed to pass on a
# status string alone: the journal must carry the reason.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-$ROOT/build/megas-headless}"
WORK="$(mktemp -d /tmp/megas-evidence-XXXXXX)"
REAL_HOME="${HOME}"
HOME_DIR="$WORK/home"
RUNTIME="$WORK/runtime"
DISK="$WORK/winlpe.qcow2"
PORT=49691
QMP=49692
VNC=49693
FAILURES=0
mkdir -p "$HOME_DIR" "$RUNTIME"
export HOME="$HOME_DIR"
export XDG_CONFIG_HOME="$HOME_DIR/.config"

pass() { printf 'PASS %s\n' "$1"; }
fail() { printf 'FAIL %s: %s\n' "$1" "$2"; FAILURES=$((FAILURES+1)); }

cleanup() {
  "$BINARY" --runtime "$RUNTIME" --port "$PORT" send '{"action":"stop"}' >/dev/null 2>&1
  sleep 0.3
  pkill -f "megas-evidence" >/dev/null 2>&1
  rm -rf "$WORK"
}
trap cleanup EXIT

owner() {
  "$BINARY" --runtime "$RUNTIME" --port "$PORT" --qmp-port "$QMP" --vnc-port "$VNC" serve \
    >"$WORK/owner.log" 2>&1 &
  for _ in $(seq 1 50); do
    [ -f "$RUNTIME/session-$PORT.token" ] && return 0
    sleep 0.1
  done
  return 1
}

send() {
  "$BINARY" --runtime "$RUNTIME" --port "$PORT" send "$1" 2>/dev/null
}

field() { python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('$1',''))"; }

journal_has() {
  python3 - "$RUNTIME/actions.jsonl" "$1" <<'PY'
import json, sys
path, needle = sys.argv[1], sys.argv[2]
try:
    for line in open(path, encoding='utf-8', errors='replace'):
        if needle in line:
            print(line.strip()[:400]); sys.exit(0)
except OSError:
    pass
sys.exit(1)
PY
}

if [ ! -x "$BINARY" ]; then
  echo "controller binary missing: $BINARY" >&2
  exit 2
fi
if ! command -v qemu-img >/dev/null 2>&1; then
  echo "qemu-img is required to build the evidence disk" >&2
  exit 2
fi

owner || { echo "owner did not start" >&2; cat "$WORK/owner.log" >&2; exit 2; }
printf 'owner ready on port %s\n' "$PORT"

# A small qcow2 image stands in for a real guest disk. The controller's
# decisions are about paths, profiles and refusals, not about disk content.
qemu-img create -f qcow2 "$DISK" 1G >/dev/null 2>&1 \
  || { echo "could not create the evidence disk" >&2; exit 2; }

# --- Fault 2: a start with no disk must be refused, and the journal must
# --- carry the reason that says so.
reply="$(send '{"action":"start","ram_mb":2048,"bus":"ide"}')"
if [ "$(printf '%s' "$reply" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("ok"))')" = "False" ]; then
  pass "empty_disk_start_is_refused"
else
  fail "empty_disk_start_is_refused" "the controller accepted a start with no disk"
fi
if journal_has start_refused >/dev/null; then
  pass "empty_disk_refusal_is_journaled"
  journal_has start_refused | sed 's/^/  journal: /'
else
  fail "empty_disk_refusal_is_journaled" "no start_refused entry in the journal"
fi

# --- Fault 3: a resize while the VM is stopped must be refused, and the
# --- journal must carry a non-empty reason.
reply="$(send '{"action":"resize","width":1280,"height":720}')"
if [ "$(printf '%s' "$reply" | python3 -c 'import json,sys;print(json.load(sys.stdin).get("ok"))')" = "False" ]; then
  pass "stopped_vm_refuses_resize"
else
  fail "stopped_vm_refuses_resize" "the controller accepted a resize while the VM was stopped"
fi
if journal_has '"action_failed"' >/dev/null; then
  reason="$(journal_has '"action_failed"' | python3 -c 'import json,sys
raw=sys.stdin.read()
try:
    outer=json.loads(raw)
    inner=json.loads(outer.get("data","{}"))
    print(inner.get("reason",""))
except Exception:
    print("")' 2>/dev/null)"
  if [ -n "$reason" ]; then
    pass "resize_refusal_reason_is_journaled"
    printf '  journal reason: %s\n' "$reason"
  else
    fail "resize_refusal_reason_is_journaled" "the journal entry carries no reason"
  fi
else
  fail "resize_refusal_reason_is_journaled" "no action_failed entry in the journal"
fi

# --- The registry names a VM once. A disk that is started under two names
# --- must stay one record.
send "{\"action\":\"start\",\"disk\":\"$DISK\",\"ram_mb\":2048,\"bus\":\"ide\",\"width\":800,\"height\":600}" >/dev/null
send '{"action":"stop"}' >/dev/null
send "{\"action\":\"start\",\"name\":\"4GB Windows\",\"disk\":\"$DISK\",\"ram_mb\":2048,\"bus\":\"ide\",\"width\":800,\"height\":600}" >/dev/null
sleep 0.5
REGISTRY="$HOME_DIR/.config/megas/vms.json"
if [ -f "$REGISTRY" ]; then
  count="$(python3 -c "import json;print(len(json.load(open('$REGISTRY')).get('vms',[])))" 2>/dev/null)"
  if [ "${count:-0}" = "1" ]; then
    pass "one_registry_record_per_vm"
    sed 's/^/  registry: /' "$REGISTRY" | head -20
  else
    fail "one_registry_record_per_vm" "the same disk produced $count registry records"
    sed 's/^/  registry: /' "$REGISTRY" | head -20
  fi
else
  fail "one_registry_record_per_vm" "the registry was not written at $REGISTRY"
fi

# --- The real HOME must not be touched by any of this.
if [ -f "$REAL_HOME/.config/megas/vms.json" ]; then
  fail "real_home_is_untouched" "the evidence run wrote into the real home directory"
else
  pass "real_home_is_untouched"
fi

send '{"action":"stop"}' >/dev/null
printf '\n'
if [ "$FAILURES" -eq 0 ]; then
  echo "evidence: every check passed"
  exit 0
fi
echo "evidence: $FAILURES check(s) failed"
exit 1
