#!/usr/bin/env bash
set -euo pipefail

list_audio_nodes() {
  pw-dump | python3 -c '
import json, sys
data = json.load(sys.stdin)
for node in data:
    if node.get("type") != "PipeWire:Interface:Node":
        continue
    props = node.get("info", {}).get("props", {})
    media = props.get("media.class", "")
    if "Audio" not in media:
        continue
    print("%s\t%s\t%s\tpos=%s" % (node.get("id"), media, props.get("node.name"), props.get("audio.position")))
'
}

first_source_name() {
  pw-dump | python3 -c '
import json, sys
data = json.load(sys.stdin)
nodes = []
for node in data:
    if node.get("type") != "PipeWire:Interface:Node":
        continue
    props = node.get("info", {}).get("props", {})
    media = props.get("media.class", "")
    name = str(props.get("node.name", ""))
    if media == "Audio/Source" or name.endswith(".monitor"):
        nodes.append(name)
for name in nodes:
    if "repro" in name:
        print(name)
        raise SystemExit
if nodes:
    print(nodes[0])
'
}

echo "=== versions ==="
pipewire --version || true
dpkg -l pipewire wireplumber libpipewire-0.3-0 2>/dev/null | awk '/^ii/{print $1,$2,$3}' || true

echo "=== kernel modules ==="
uname -a
ls /lib/modules/"$(uname -r)"/kernel/sound/drivers/ 2>/dev/null || true
sudo modprobe snd-dummy 2>/dev/null || echo "snd-dummy: not loaded"
sudo modprobe snd-aloop 2>/dev/null || echo "snd-aloop: not loaded"
ls -l /dev/snd 2>/dev/null || true

echo "=== start pipewire ==="
pipewire &
PIPEWIRE_PID=$!
pipewire-pulse &
PULSE_PID=$!
wireplumber &
WIREPLUMBER_PID=$!
sleep 2
pw-cli info 0

echo "=== create stereo null sink ==="
pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=repro-sink media.class=Audio/Sink object.linger=true audio.channels=2 audio.position=[FL,FR] }' || true
if command -v pactl >/dev/null; then
  pactl load-module module-null-sink sink_name=repro-pulse channels=2 channel_map=front-left,front-right || true
fi
sleep 1

echo "=== nodes before playback ==="
list_audio_nodes || true

gcc -O2 -o /tmp/repro-issue-169-capture scripts/repro-issue-169-capture.c \
  $(pkg-config --cflags --libs libpipewire-0.3) -pthread

ffmpeg -nostdin -y -f lavfi -i sine=frequency=440:sample_rate=48000:duration=8 -ac 2 /tmp/sine.wav >/tmp/ffmpeg.log 2>&1
pw-cat --playback --rate 48000 --channels 2 --target repro-sink /tmp/sine.wav >/tmp/pw-cat.log 2>&1 &
PLAY_PID=$!
sleep 1

echo "=== nodes during playback ==="
list_audio_nodes || true

MONITOR="$(first_source_name || true)"
echo "capture target: ${MONITOR:-default}"

echo "=== UNKNOWN position ==="
if [[ -n "${MONITOR}" ]]; then
  /tmp/repro-issue-169-capture unknown "${MONITOR}" | tee /tmp/unknown.out
else
  /tmp/repro-issue-169-capture unknown | tee /tmp/unknown.out
fi

echo "=== MONO position ==="
if [[ -n "${MONITOR}" ]]; then
  /tmp/repro-issue-169-capture mono "${MONITOR}" | tee /tmp/mono.out
else
  /tmp/repro-issue-169-capture mono | tee /tmp/mono.out
fi

echo "=== pw-link ==="
pw-link -l || true

kill "${PLAY_PID}" 2>/dev/null || true
kill "${WIREPLUMBER_PID}" "${PULSE_PID}" "${PIPEWIRE_PID}" 2>/dev/null || true

echo "=== summary ==="
echo "unknown $(cat /tmp/unknown.out)"
echo "mono    $(cat /tmp/mono.out)"
