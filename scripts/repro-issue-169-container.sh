#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
if ! command -v pipewire >/dev/null || ! command -v gcc >/dev/null; then
  apt-get update -qq
  apt-get install -y -qq \
    pipewire wireplumber pipewire-pulse pipewire-bin \
    libpipewire-0.3-dev gcc pkg-config ffmpeg python3 \
    dbus dbus-user-session alsa-utils >/tmp/apt.log
fi

echo "=== versions ==="
pipewire --version
dpkg -l pipewire wireplumber libpipewire-0.3-0 | awk '/^ii/{print $1,$2,$3}'

echo "=== aloop ==="
modprobe snd-aloop 2>/dev/null || true
cat /proc/asound/cards
ls -l /dev/snd

echo "=== alsa loopback smoke ==="
ffmpeg -nostdin -y -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 -ac 2 /tmp/sine-short.wav >/dev/null 2>&1
aplay -D hw:Loopback,0,0 -q /tmp/sine-short.wav &
arecord -D hw:Loopback,1,0 -f S16_LE -c 2 -r 48000 -d 1 /tmp/aloop.wav >/dev/null 2>&1 || true
wait || true
python3 - <<'PY'
import wave, sys
try:
    w = wave.open("/tmp/aloop.wav")
    frames = w.readframes(w.getnframes())
    peak = max(abs(int.from_bytes(frames[i:i+2], "little", signed=True)) for i in range(0, len(frames)-1, 2)) if frames else 0
    print("alsa_loopback_bytes", len(frames), "peak", peak)
except Exception as e:
    print("alsa_loopback_failed", e)
PY

gcc -O2 -o /tmp/repro-issue-169-capture /src/scripts/repro-issue-169-capture.c \
  $(pkg-config --cflags --libs libpipewire-0.3) -pthread

ffmpeg -nostdin -y -f lavfi -i sine=frequency=440:sample_rate=48000:duration=8 -ac 2 /tmp/sine.wav >/dev/null 2>&1

export XDG_RUNTIME_DIR=/tmp/pw-runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

dbus-run-session -- bash -s <<'EOF'
set -euo pipefail
pipewire &
sleep 0.4
pipewire-pulse &
wireplumber &
sleep 3
pw-cli info 0 >/dev/null
echo "=== create alsa loopback nodes ==="
LOOP_CARD=$(awk '/\[Loopback/{print $1}' /proc/asound/cards)
echo "loop_card=$LOOP_CARD"
timeout 5 pw-cli create-node adapter "{ factory.name=api.alsa.pcm.sink node.name=aloop-sink media.class=Audio/Sink audio.device=\"hw:${LOOP_CARD},0\" object.linger=true }" || true
timeout 5 pw-cli create-node adapter "{ factory.name=api.alsa.pcm.source node.name=aloop-source media.class=Audio/Source audio.device=\"hw:${LOOP_CARD},1\" object.linger=true }" || true
sleep 1

python3 - <<'PY'
import json, subprocess
data = json.loads(subprocess.check_output(["pw-dump"]))
print("=== nodes ===")
for node in data:
    if node.get("type") != "PipeWire:Interface:Node":
        continue
    props = node.get("info", {}).get("props", {})
    media = props.get("media.class", "")
    if "Audio" not in str(media):
        continue
    print(node.get("id"), media, props.get("node.name"), "pos=", props.get("audio.position"), "alsa=", props.get("api.alsa.path", ""))
PY

SINK=$(python3 - <<'PY'
import json, subprocess
data = json.loads(subprocess.check_output(["pw-dump"]))
for node in data:
    if node.get("type") != "PipeWire:Interface:Node":
        continue
    props = node.get("info", {}).get("props", {})
    name = str(props.get("node.name", ""))
    media = props.get("media.class", "")
    if media == "Audio/Sink" and "Loopback" in name:
        print(name)
        break
else:
    for node in data:
        if node.get("type") != "PipeWire:Interface:Node":
            continue
        props = node.get("info", {}).get("props", {})
        if props.get("media.class") == "Audio/Sink":
            print(props.get("node.name", ""))
            break
PY
)

SOURCE=$(python3 - <<'PY'
import json, subprocess
data = json.loads(subprocess.check_output(["pw-dump"]))
for node in data:
    if node.get("type") != "PipeWire:Interface:Node":
        continue
    props = node.get("info", {}).get("props", {})
    name = str(props.get("node.name", ""))
    media = props.get("media.class", "")
    if media == "Audio/Source" and "Loopback" in name:
        print(name)
        break
else:
    for node in data:
        if node.get("type") != "PipeWire:Interface:Node":
            continue
        props = node.get("info", {}).get("props", {})
        if props.get("media.class") == "Audio/Source":
            print(props.get("node.name", ""))
            break
PY
)

echo "sink=$SINK"
echo "source=$SOURCE"

if [[ -n "$SINK" ]]; then
  pw-cat --playback --rate 48000 --channels 2 --target "$SINK" /tmp/sine.wav >/tmp/pw-cat.log 2>&1 &
else
  pw-cat --playback --rate 48000 --channels 2 /tmp/sine.wav >/tmp/pw-cat.log 2>&1 &
fi
PLAY_PID=$!
sleep 1

echo "=== UNKNOWN ==="
if [[ -n "$SOURCE" ]]; then
  /tmp/repro-issue-169-capture unknown "$SOURCE" | tee /tmp/unknown.out
else
  /tmp/repro-issue-169-capture unknown | tee /tmp/unknown.out
fi

echo "=== MONO ==="
if [[ -n "$SOURCE" ]]; then
  /tmp/repro-issue-169-capture mono "$SOURCE" | tee /tmp/mono.out
else
  /tmp/repro-issue-169-capture mono | tee /tmp/mono.out
fi

echo "=== pw-link ==="
pw-link -l || true
kill "$PLAY_PID" 2>/dev/null || true
echo "=== summary ==="
echo "unknown $(cat /tmp/unknown.out)"
echo "mono    $(cat /tmp/mono.out)"
EOF
