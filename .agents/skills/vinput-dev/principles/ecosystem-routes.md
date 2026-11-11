# Ecosystem Repositories: vinput-registry, aur-auto & flatpak-auto

---

## 1. Working with `vinput-registry` ([xifan2333/vinput-registry](https://github.com/xifan2333/vinput-registry), `~/Code/vinput-registry`)

When adding or updating ASR providers, LLM scenes, or local model catalogs, work in `~/Code/vinput-registry`.

### Repository Layout
- `registry/`: Index metadata files (`models.json`, `providers.json`, `adapters.json`).
- `resources/providers/<folder>/<name>/`: Cloud ASR provider scripts (`entry.py` + `README.md`).
- `resources/adapters/<folder>/<name>/`: LLM scene adapter scripts (`entry.py` + `README.md`).
- `i18n/`: Multilingual display text (`en_US.json`, `zh_CN.json`).

### Rules for Scripts & Adapters
1. **Self-contained**: Must use Python 3 standard library only (`urllib.request`, `json`, `hashlib`, etc.) - zero pip dependencies.
2. **Resource IDs**: Stable machine identifiers following `<kinds>.<folder>.<name>` (e.g., `providers.doubao.asr-stream`).
3. **Short ID**: Human-readable identifier for CLI/GUI display (e.g., `doubao-stream`).
4. **Environment Variables**: Use standard `VINPUT_ASR_*` namespaces:
   - `VINPUT_ASR_API_KEY`: API credential / Bearer token
   - `VINPUT_ASR_APP_ID`: App ID (if required)
   - `VINPUT_ASR_URL` / `VINPUT_ASR_BASE_URL`: Endpoint overrides
   - `VINPUT_ASR_MODEL`: Remote model name
   - `VINPUT_ASR_LANGUAGE`: Language hint
   - `VINPUT_ASR_PROMPT`: Context / prompt bias
5. **i18n Keys**: Add `<id>.title` and `<id>.description` into both `i18n/en_US.json` and `i18n/zh_CN.json`.

---

## 2. Working with `aur-auto` ([xifan2333/aur-auto](https://github.com/xifan2333/aur-auto), `~/Code/aur-auto`)

Arch Linux binary package (`fcitx5-vinput-bin`) publication is automated in `~/Code/aur-auto`.

### Package Structure (`pkgs/fcitx5-vinput-bin/`)
- `upstream.sh`: Fetches the latest release version and asset download URLs from `xifan2333/fcitx5-vinput` on GitHub.
- `PKGBUILD`: Template for extracting pre-built binaries, installing systemd user units, icons, desktop files, and licenses.
- `fcitx5-vinput-bin.install`: Post-install hooks (reminding users to reload systemd user daemon).

### Release Flow to AUR
1. A new GitHub release tag is pushed in `fcitx5-vinput` (`mise run release <version>`).
2. GitHub Actions in `aur-auto` triggers `build-and-publish.yml`.
3. `aur-auto` runs `upstream.sh` to fetch release assets, generates updated `PKGBUILD`, builds the package in a clean chroot container, and pushes the commit directly to the AUR Git repo.

---

## 3. Working with `flatpak-auto` ([xifan2333/flatpak-auto](https://github.com/xifan2333/flatpak-auto), `~/Code/flatpak-auto`)

Flatpak OSTree repository synchronization and publishing for `org.fcitx.Fcitx5.Addon.Vinput` is managed in `~/Code/flatpak-auto`.

### Product Structure (`products/fcitx5-vinput/`)
- `upstream.sh`: Detects new releases of `fcitx5-vinput` and resolves download links for `fcitx5-vinput.flatpak` bundle.
- `product.json`: Product metadata (app id, name, summary, description).

### Release Flow to Flatpak Repository
1. GitHub Actions workflow `sync-and-publish.yml` triggers upon release detection.
2. Downloads the pre-built `fcitx5-vinput.flatpak` bundle artifact from `fcitx5-vinput`.
3. Imports the bundle into the shared OSTree repository (`xifan` remote).
4. Generates updated `.flatpakrepo` and `.flatpakref` files and publishes them to GitHub Pages.
