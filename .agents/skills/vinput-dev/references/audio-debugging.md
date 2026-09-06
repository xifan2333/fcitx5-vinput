# Audio Subsystem & Daemon Debugging

---

## 1. Inspecting Daemon Logs

```bash
# Follow live daemon logs via systemd user service
journalctl --user -u vinput-daemon.service -f -n 100
# or via CLI
vinput daemon logs -f

# Run daemon directly in foreground for interactive debugging
vinput daemon stop
VINPUT_DEBUG=1 vinput-daemon

# Or enable debug logging for the systemd user service
systemctl --user set-environment VINPUT_DEBUG=1
vinput daemon restart
vinput daemon logs -f
```

---

## 2. Benchmarking Audio Capture Cold-Start

To measure PipeWire stream connection latency and cold-start time:

```bash
bash scripts/bench-capture-cold-start.sh
```

---

## 3. Common Diagnostic Commands

```bash
# Verify vinput D-Bus service availability
gdbus introspect --session --dest org.fcitx.Vinput --object-path /org/fcitx/Vinput

# Trigger recording via CLI
vinput toggle
vinput status
```
