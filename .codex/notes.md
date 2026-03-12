# Review Notes

Date: 2026-03-12

- Breaking change: legacy scene file `~/.config/fcitx5/conf/vinput-scenes.json` is no longer read.
- Scenes must be configured under `scenes` in `~/.config/vinput/config.json` for menu/LLM prompts to work.
- No compatibility or migration will be added; document this in the release notes.
- Review: CoreConfig::Scenes.activeScene defaults to "default" but built-in scenes were removed; if `scenes` is missing, activeScene points to a non-existent id and Resolve returns empty, breaking menu/LLM until user sets active.
- Review: AddScene does not repair invalid/empty activeSceneId; first added scene may still leave active invalid unless user runs "scene use".
- Review: RemoveScene now blocks deleting active scene unless forced; behavior change for CLI scripts, document or adjust.
- Review: Scenes loaded from `config.json` are no longer validated; missing/empty `id` or duplicate ids will now be accepted, leading to blank menu entries and inconsistent CLI/GUI behavior. Old loader skipped invalid/duplicate scenes.
- Review: A malformed scene entry (non-object or wrong types) in `scenes.definitions` can now throw during `CoreConfig` deserialization and drop the entire config (LoadCoreConfig returns defaults). Old loader skipped bad entries with warnings.
- Review: Any invalid item in `scenes.definitions` (wrong type) can throw during JSON deserialization, causing the entire config load to fail and fall back to defaults.
- Review: Duplicate or empty scene ids are no longer filtered; this can create blank menu rows and unpredictable selection behavior.
- Review: CLI `llm remove --force` removes the active provider but does not update `active_provider`, leaving it pointing to a missing entry.
- Review: GUI uses `vinput daemon reload` after saving settings, but CLI has no `daemon reload` subcommand; config changes won’t be applied as intended.
- Review: GUI `refreshDaemonStatus` calls `vinput status --json`, but the CLI returns exit code 1 when the daemon is stopped; `RunVinputJson` treats non-zero exit as error, so GUI shows an error instead of “Stopped”.
- Review: README is out of sync with current config model (still documents only `vinput.conf` and 8 options, legacy LLM fields, built-in scenes, and “only paraformer” support). Code now uses `config.json`, LLM providers, custom scenes, and multiple model types.
- Review: ARCHITECTURE.md references `scene.json` and a CLI `scene add --type` regression that no longer exists; doc likely stale after scene model refactor.
- Review: CLI help for `config get` suggests `fcitx.triggerKey` paths, but implementation only supports `extra.*` (config.json). Users will get an error for `fcitx.*`.
