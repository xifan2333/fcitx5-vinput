# Direct Desktop Notifications and Error Classification Abolition

`vinput-daemon` dispatches desktop notifications directly to `org.freedesktop.Notifications` over the user session D-Bus, bypassing the Fcitx5 addon entirely. The legacy `ErrorInfo` / `kErrorCode*` / `ClassifyErrorText` string-parsing and classification system is abolished in favor of direct localized text from the daemon and verbatim notification lines from helper processes.

## Context

Historically, `vinput` routed all notifications through the Fcitx5 input method addon:
1. Helper processes (ASR streaming/batch, LLM adapters) wrote errors or diagnostics to stderr.
2. The daemon formatted these into English strings, passed them through `ClassifyErrorText` (300+ lines of regex and prefix parsers) to reconstruct synthetic error codes (`kErrorCode*`), and emitted a D-Bus signal (`DaemonNotification`).
3. The Fcitx5 addon listened on D-Bus, matched error codes through a 300-line dictionary (`RenderErrorMessage`) to reconstruct localized messages, and invoked `fcitx::INotifications::sendNotification`.
4. The addon also intercepted notifications to abort active input sessions (`finishFrontendSession`), coupling UI state machine transitions to notification delivery.
5. Standalone CLI and GUI commands made D-Bus calls into the Fcitx5 addon object `/org/fcitx/Fcitx5/Vinput` to display notifications, failing whenever Fcitx5 was not active.

Because `vinput-daemon` is a systemd user service with its own connection to the D-Bus user session bus (`sd-bus`), bouncing notifications through the Fcitx5 addon introduced ~750 lines of accidental complexity and cross-process fragility.

## Decision

1. **Direct Notification Pipeline**: `vinput-daemon` owns desktop notifications. `NotificationBridge` inside `vinput-daemon` consumes helper `stderr`, parses notification lines (`{"title": ..., "body": ...}` or verbatim plain text), collapses duplicates across a short sliding window, and calls `org.freedesktop.Notifications.Notify` directly via asynchronous `sd-bus`.
2. **Abolition of Error Classification**: Delete `src/common/dbus/error_info.{h,cpp}`, `kErrorCode*` constants, `ClassifyErrorText`, and `RenderErrorMessage`. Daemon internal errors produce localized `(title, body)` directly via gettext; helper processes own their wording (inheriting `LANG`/`LC_ALL`).
3. **Decoupled Frontend Lifecycle**: The Fcitx5 addon removes its notifier D-Bus object (`VinputNotifierDBusObject`) and notification listener. Addon input state (preedit, commit) is driven strictly by `StatusChanged` (`idle`, `recording`, `inferring`, `postprocessing`, `error`).

## Consequences

- Helper notifications appear reliably even if Fcitx5 is restarted or not running.
- Eliminates brittle regex string-parsing of error messages.
- Net removal of ~750 lines of boilerplate across daemon, common, and addon modules.
- Helper scripts have a transparent contract: printing a line to `stderr` displays a desktop notification popup.
