# Audio Subsystem & Daemon Debugging

---

## 1. Inspecting Daemon Logs

```bash
# Follow live daemon logs via systemd user service
journalctl --user -u vinput-daemon.service -f -n 100
# or via CLI
vinput daemon logs -f

# Run daemon directly in foreground with debug/trace logging
vinput daemon stop
VINPUT_LOG_LEVEL=debug vinput-daemon
# or via flag
vinput-daemon --log-level debug

# Enable debug logging for the systemd user service
systemctl --user set-environment VINPUT_LOG_LEVEL=debug
vinput daemon restart
vinput daemon logs -f
```

---

## 2. Inspecting Audio Capture Stream Events

To inspect PipeWire stream connection events and timing, enable debug logging and search journal:

```bash
# Search capture events in journal
journalctl --user -u vinput-daemon.service -g "capture"
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
