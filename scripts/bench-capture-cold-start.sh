#!/usr/bin/env bash
# Baseline / A-B helper for cold PTT capture latency (issue #112).
#
# Does NOT drive the mic automatically. You perform Tap trials; this script
# scrapes vinput-daemon journal lines produced under VINPUT_DEBUG=1.
#
# Required daemon logs (from P0.1+):
#   [vinput-debug] capture begin idle_gap_ms=... create_stream_ms=... set_active_ms=...
#   [vinput-debug] start timing ... session_ms=... create_stream_ms=... stream_reused=...
#   [vinput-debug] capture first buffer after N ms
#   [vinput-debug] first non-silent audio after N ms
#   [vinput-debug] capture first_buffer_ms=N (at stop)
#
# Usage:
#   # ensure debug is on for the user unit, then restart:
#   # mkdir -p ~/.config/systemd/user/vinput-daemon.service.d
#   # printf '%s\n' '[Service]' 'Environment=VINPUT_DEBUG=1' \
#   #   > ~/.config/systemd/user/vinput-daemon.service.d/debug.conf
#   # systemctl --user daemon-reload && systemctl --user restart vinput-daemon
#
# Protocols (manual):
#   Cold: wait >=10s idle, Tap, speak immediately, stop. Repeat N>=30.
#   Warm: stop then re-Tap within <2s. Repeat N>=20.
#
# Scrape last 24h and print distributions:
#   ./scripts/bench-capture-cold-start.sh
#   ./scripts/bench-capture-cold-start.sh --since "2 hours ago"
#   ./scripts/bench-capture-cold-start.sh --follow   # tail live

set -euo pipefail

SINCE="24 hours ago"
FOLLOW=0

while [[ $# -gt 0 ]]; do
  case "$1" in
  --since)
    SINCE="${2:?}"
    shift 2
    ;;
  --follow | -f)
    FOLLOW=1
    shift
    ;;
  -h | --help)
    sed -n '2,40p' "$0"
    exit 0
    ;;
  *)
    echo "unknown arg: $1" >&2
    exit 2
    ;;
  esac
done

UNIT="vinput-daemon.service"

if [[ "$FOLLOW" -eq 1 ]]; then
  exec journalctl --user -u "$UNIT" -f --no-pager |
    rg --line-buffered 'capture begin|start timing|first buffer|first non-silent|VAD trimmed|negotiated format|recording started|recording stopped'
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

journalctl --user -u "$UNIT" --since "$SINCE" --no-pager 2>/dev/null |
  rg 'first non-silent audio after|capture first buffer after|capture begin idle_gap|start timing |capture first_buffer_ms=|VAD trimmed' \
    >"$TMP" || true

python3 - "$TMP" <<'PY'
import re, sys
from statistics import mean, median

path = sys.argv[1]
text = open(path, errors="replace").read().splitlines()

def stats(name, vals):
    if not vals:
        print(f"{name}: (no samples)")
        return
    s = sorted(vals)
    def pct(p):
        i = min(len(s) - 1, max(0, int(round((p / 100) * (len(s) - 1)))))
        return s[i]
    slow = sum(1 for x in vals if x >= 350)
    fast = sum(1 for x in vals if x < 150)
    print(
        f"{name}: n={len(vals)} min={min(vals)} p25={pct(25)} median={median(vals):.0f} "
        f"p75={pct(75)} p90={pct(90)} max={max(vals)} mean={mean(vals):.1f} "
        f"fast<150={fast}({fast/len(vals)*100:.1f}%) slow>=350={slow}({slow/len(vals)*100:.1f}%)"
    )

first_ns = []
first_buf = []
create_ms = []
session_ms = []
idle_gaps = []
vad_removed_ms = []

for line in text:
    m = re.search(r"first non-silent audio after (-?\d+) ms", line)
    if m:
        first_ns.append(int(m.group(1)))
    m = re.search(r"capture first buffer after (-?\d+) ms", line)
    if m:
        first_buf.append(int(m.group(1)))
    m = re.search(r"capture first_buffer_ms=(-?\d+)", line)
    if m and int(m.group(1)) >= 0:
        first_buf.append(int(m.group(1)))
    m = re.search(r"create_stream_ms=(-?\d+)", line)
    if m and int(m.group(1)) >= 0:
        create_ms.append(int(m.group(1)))
    m = re.search(r"session_ms=(-?\d+)", line)
    if m and int(m.group(1)) >= 0:
        session_ms.append(int(m.group(1)))
    m = re.search(r"idle_gap_ms=(-?\d+)", line)
    if m and int(m.group(1)) >= 0:
        idle_gaps.append(int(m.group(1)))
    m = re.search(r"VAD trimmed (\d+) -> (\d+) samples", line)
    if m:
        removed = int(m.group(1)) - int(m.group(2))
        vad_removed_ms.append(removed / 16.0)

print("=== vinput capture cold-start scrape ===")
stats("first_non_silent_ms", first_ns)
stats("first_buffer_ms", first_buf)
stats("create_stream_ms", create_ms)
stats("session_ms", session_ms)
stats("idle_gap_ms", idle_gaps)
stats("vad_removed_ms", [int(round(x)) for x in vad_removed_ms])

# Success targets from issue #112 (cold cohort is manual; overall scrape is a hint)
if first_ns:
    med = median(first_ns)
    slow = sum(1 for x in first_ns if x >= 350) / len(first_ns) * 100
    print()
    print("Issue #112 targets (for idle>=10s Tap cohort, not full scrape):")
    print("  first_ns median <= 120ms")
    print("  slow>=350 share <= 15%")
    print(f"Current scrape (mixed gaps): median={med:.0f}ms slow_share={slow:.1f}%")
PY
