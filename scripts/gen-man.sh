#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

if ! command -v pandoc >/dev/null 2>&1; then
    echo "Error: pandoc is required to generate man pages." >&2
    exit 1
fi

echo "Generating man pages with pandoc..."

for lang in en zh_CN; do
    echo "  Processing [${lang}]..."
    if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
        if [ "${lang}" = "zh_CN" ]; then
            man_date="$(date -d "@${SOURCE_DATE_EPOCH}" +'%Y年%-m月' 2>/dev/null || date -r "${SOURCE_DATE_EPOCH}" +'%Y年%-m月' 2>/dev/null || date +'%Y年%-m月')"
        else
            man_date="$(LC_TIME=C date -d "@${SOURCE_DATE_EPOCH}" +'%B %Y' 2>/dev/null || LC_TIME=C date -r "${SOURCE_DATE_EPOCH}" +'%B %Y' 2>/dev/null || LC_TIME=C date +'%B %Y')"
        fi
    else
        if [ "${lang}" = "zh_CN" ]; then
            man_date="$(date +'%Y年%-m月')"
        else
            man_date="$(LC_TIME=C date +'%B %Y')"
        fi
    fi

    for name in vinput.1 vinput-daemon.1 vinput-gui.1 vinput-config.5 fcitx5-vinput.7; do
        pandoc --from=markdown-smart -s -t man -V date="${man_date}" \
            "${ROOT_DIR}/data/man/${lang}/${name}.md" \
            -o "${ROOT_DIR}/data/man/${lang}/${name}"
    done
done

echo "Man pages generated successfully."
