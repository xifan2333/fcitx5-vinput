# Changelog

All notable changes to this project will be documented in this file.

## [2.0.3](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v2.0.3) — 2026-03-30

### Bug Fixes

- **models:** Accept tokenizer-based metadata for `funasr_nano` and `qwen3_asr` local models
- **sherpa-offline:** Only pass `model.tokens` when the model family actually uses it

### Features

- **gui:** Add visible download status/progress UI for resource downloads
- **downloads:** Report transfer speed and throttle duplicate progress updates

## [2.0.2](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v2.0.2) — 2026-03-30

### Bug Fixes

- **command:** Fix null command scene lookup in command mode

## [2.0.1](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v2.0.1) — 2026-03-30

### Bug Fixes

- **streaming:** Accumulate committed segments for multi-segment ASR

### Features

- **addon:** Add unified ASR provider / model selection menu with configurable `ASR Menu Keys` (`F8` by default)

### Internationalization

- Add translations for ASR menu strings

## [1.1.1](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.1.1) — 2026-03-19

### Features

- **gui:** Add candidate count to scene dialog, cache model list
## [1.1.0](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.1.0) — 2026-03-18

### Miscellaneous

- Bump version to 1.1.0, add changelog generation

### Refactor

- Per-scene provider/candidates + i18n fixes + check-i18n.py
- Per-scene provider binding + command scene unification
## [1.0.20](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.20) — 2026-03-18

### Bug Fixes

- **cli:** Allow recording stop without active scene, output raw ASR result

### Documentation

- Add AUR, COPR, PPA installation instructions
## [1.0.19](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.19) — 2026-03-18

### Bug Fixes

- **ci:** Use new passphrase-free GPG key for PPA signing
- **ci:** Use --no-tty with loopback pinentry for debsign
- **ci:** Use debsign with explicit GPG batch/loopback flags for PPA signing
- **ci:** Remove batch mode from GPG config, conflicts with loopback pinentry
- **ci:** Add batch mode to GPG config for headless signing
- **ci:** Use dpkg-buildpackage with direct GPG signing for PPA
- **ci:** Use publish-ppa-package action for PPA upload
- **ci:** Remove --batch from GPG wrapper to allow signing
- **ci:** Fix GPG wrapper script heredoc indentation
- **ci:** Use GPG wrapper script for headless debsign
- **ci:** Configure gpg.conf for loopback pinentry
- **ci:** Pass empty passphrase for GPG signing
- **ci:** Separate debuild and debsign for headless GPG
- **ci:** Fix debuild lintian flag ordering
- **ci:** Fix GPG signing in headless CI for PPA upload
- **ci:** Provide orig tarball for PPA source package
- **debian:** Remove conflicting compat file
- **spec:** Correct addon .so filename and remove nonexistent inputmethod conf
- **ci:** Skip build-deps check for PPA source package
- **ci:** Use template version in spec for COPR compatibility
- **ci:** Install copr-cli via pip instead of apt

### CI

- Add PPA upload job and debian packaging files
- Add COPR build job and Fedora spec file
- Add build workflow on push/PR and CI badge

### Documentation

- Update demo video filename
- Update demo video
- Use GitHub user-attachments URL for demo video
- Fix demo video URL and clean up features list
- Update trigger modes in README (tap/hold/CLI)
- Add demo video, issue templates, and contributing guide
- Add AUR downloads badge to README

### Miscellaneous

- Bump version to 1.0.19
- Remove unused assets/demo.mp4
## [1.0.18](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.18) — 2026-03-17

### Features

- **asr:** Add decoding params, peak normalization, and VAD trimming
## [1.0.17](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.17) — 2026-03-17

### Bug Fixes

- **addon:** Support toggle-off on second keypress and track result state
- **cli:** Fix recording toggle reporting start instead of stop
- **daemon:** Prevent capturing desktop audio via PipeWire
## [1.0.16](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.16) — 2026-03-17

### Bug Fixes

- **gui:** Use system palette colors and right-align size column

### Documentation

- Fix AUR badge package name
- Beautify README for English and Chinese versions

### Features

- **cli:** Add recording subcommand with D-Bus control

### Refactor

- Extract shared utilities and deduplicate CLI helpers
## [1.0.15](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.15) — 2026-03-15

### Bug Fixes

- Use character offsets and validate UTF-8 for command mode selected text
## [1.0.14](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.14) — 2026-03-15

### Refactor

- Unify payload format and add candidate count prompt injection
## [1.0.13](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.13) — 2026-03-13

### Bug Fixes

- Show result menu only when LLM returns more than one candidate
## [1.0.12](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.12) — 2026-03-13

### Documentation

- Split README into separate English and Chinese versions

### Refactor

- Unify LLM response to JSON format and simplify candidate count logic
## [1.0.11](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.11) — 2026-03-13

### Bug Fixes

- Create model base directory if not exists before mkdtemp
## [1.0.10](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.10) — 2026-03-13

### Bug Fixes

- Auto-commit command result when candidate count is 1
## [1.0.9](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.9) — 2026-03-13

### Bug Fixes

- Improve LLM processing logic and prevent duplicate requests
- Prevent use-after-free crash when InputContext is destroyed
## [1.0.8](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.8) — 2026-03-12

### Bug Fixes

- **addon:** Guard setComment with version check for fcitx5 < 5.1.9
## [1.0.7](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.7) — 2026-03-12

### Features

- LLM error notifications, force-remove model/scene, fix GUI download log
## [1.0.5](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.5) — 2026-03-12

### Bug Fixes

- **ci:** Add git to Debian 12 build deps for CLI11 FetchContent clone
## [1.0.4](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.4) — 2026-03-12

### CI

- Add Debian 12 build job to release workflow
## [1.0.3](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.3) — 2026-03-12

### Bug Fixes

- **ci:** Add cli11 and git to Arch build deps; add cli11 to PKGBUILD makedepends
## [1.0.2](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.2) — 2026-03-12

### Bug Fixes

- **addon:** Support FCITX_ADDON_FACTORY_V2 only on fcitx5 >= 5.1.12
## [1.0.1](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.1) — 2026-03-12

### Bug Fixes

- **ci:** Move CXX_STANDARD after Fcitx5CompilerSettings; restore packaging/arch/PKGBUILD.in with updated deps
## [1.0.0](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v1.0.0) — 2026-03-12

### Bug Fixes

- **addon:** Persist active scene to config on scene switch
- **daemon:** Robustness and safety improvements
- **common:** Security and robustness fixes
- **addon:** Declare clipboard as hard dependency, fix include path
- **daemon:** Eliminate cross-thread sd-bus access via eventfd emit queue

### CI

- Add missing build deps (libarchive, openssl, qt5) to release workflow

### Documentation

- Remove outdated ARCHITECTURE.md

### Features

- **addon:** Check LLM enabled before command mode; update default keybindings
- **gui:** Replace QListWidget with QTableWidget for model lists
- **cli:** I18n support, CJK-aware table formatting, supports_hotwords column
- Add command mode with dedicated trigger key
- **gui:** Integrate daemon control into general tab and add zh_CN translations
- Complete GUI with scene/LLM tabs, unify i18n, add PipeWire device API
- Support multiple download URLs with fallback in model registry
- Add vinput CLI with init, model/scene/llm/config/daemon management
- Add support for new API endpoint and update data processing logic.
- Implement dynamic model loading via vinput-model.json metadata

### Miscellaneous

- Untrack IDE/build artifacts, update gitignore
- Ignore common backup files (*.po~, *.orig, *~)
- Remove po~ backup file, add to gitignore
- **i18n:** Update zh_CN translations
- **i18n:** Update zh_CN translations and po template
- Add .claude/ to .gitignore
- Add .cache/ and compile_commands.json to .gitignore

### Refactor

- Address all LOW-level review items
- **addon:** Conform to fcitx5 addon conventions
- **hotwords:** Replace word list with file path
- Simplify scene model, remove type and llm fields
- Remove per-provider candidate_count, use global config
- Flatten CoreConfig by removing nested "core" wrapper
- Restructure vinput as a Module addon to coexist with RIME
## [0.1.6](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v0.1.6) — 2026-03-07

### Miscellaneous

- Bump version to 0.1.6
- Add .ace-tool/ to .gitignore

### Refactor

- Normalize LLM base URL at save time instead of request-layer fallback
## [0.1.5](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v0.1.5) — 2026-03-06

### CI

- Publish releases without local git checkout
## [0.1.4](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v0.1.4) — 2026-03-06

### Bug Fixes

- Support older fcitx candidate list APIs
## [0.1.3](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v0.1.3) — 2026-03-06

### Bug Fixes

- Use legacy fcitx standard path API for compatibility
## [0.1.2](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v0.1.2) — 2026-03-06

### CI

- Add missing Debian fcitx5 utils dependency
## [0.1.1](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v0.1.1) — 2026-03-06

### Bug Fixes

- Add nlohmann_json packaging dependency
## [0.1.0](https://github.com/xifan2333/fcitx5-vinput/releases/tag/v0.1.0) — 2026-03-06

### CI

- Add GitHub Actions workflow for automated builds

### Documentation

- Document release packaging

### Features

- Initial completion of first version
