# Contributing

Thanks for your interest in fcitx5-vinput!

## Documentation

Full documentation is available at the [project site](https://xifan2333.github.io/fcitx5-vinput/). For registry contribution guidelines (ASR providers, LLM adapters, models), see the [Registry](https://xifan2333.github.io/fcitx5-vinput/registry/) page.

## Build from Source

**Dependencies:** cmake, fcitx5, pipewire, libcurl, nlohmann-json, CLI11, Qt6

```bash
sudo bash scripts/build-sherpa-onnx.sh
cmake --preset release-clang-mold
cmake --build --preset release-clang-mold
sudo cmake --install build
```

Or use `mise`:

```bash
mise run sherpa
mise run configure-release
mise run build
sudo mise run install
```

## Project Structure

- `src/addon/` — Fcitx5 addon (key events, D-Bus client, UI)
- `src/daemon/` — ASR daemon (audio capture, inference, post-processing)
- `src/common/` — Shared code (configs, D-Bus interface, scenes)
- `src/cli/` — `vinput` CLI tool
- `site/` — Documentation site (Astro + Starlight)

## Submitting Changes

1. Fork the repo and create a branch from `main`
2. Make your changes — keep commits focused and atomic
3. Test locally: enable the addon in Fcitx5, verify the daemon works
4. Open a PR with a clear description of what and why

## Reporting Bugs

Use the [Bug Report](https://github.com/xifan2333/fcitx5-vinput/issues/new?template=bug_report.yml) template. Include:
- Version (`vinput --version`)
- Distribution
- Daemon logs: `journalctl --user -u vinput-daemon.service`

## Contributing to the Registry

The [vinput-registry](https://github.com/xifan2333/vinput-registry) repository hosts downloadable resources: ASR models, cloud ASR provider scripts, and LLM adapter scripts.

To contribute a new provider, adapter, or model, see the [Registry](https://xifan2333.github.io/fcitx5-vinput/registry/) page for the full specification.

Key rules:
- Scripts must be self-contained — standard library only, no third-party dependencies
- Python 3 is recommended; other runtimes are acceptable
- Each script resource needs an `entry.py` (or equivalent) and `README.md`
- Add i18n entries for both `en_US.json` and `zh_CN.json`

## Translations

Gettext catalogs live in `po/`. The `.pot` template is not committed.
To add a language, extract strings from source and run `msginit`:

```bash
xgettext --keyword=_ --keyword=N_ --language=C++ --from-code=UTF-8 \
  --no-location --sort-by-file \
  -o - \
  $(find src/cli src/common src/addon src/daemon \( -name '*.cpp' -o -name '*.h' \)) \
| msginit --locale=ja_JP --input=- --output=po/ja.po
```

Then add the new `.po` file to `po/CMakeLists.txt` and submit a PR.

## Code Style

- C++20
- Follow the existing style in the codebase
