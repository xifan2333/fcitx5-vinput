#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
    echo "usage: $0 <template> <output> <pkgver> <source-archive> [pkgrel]" >&2
    exit 1
fi

template_path=$1
output_path=$2
pkgver=$3
source_archive=$4
pkgrel=${5:-1}

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/.." && pwd)
default_pkgurl="https://github.com/OWNER/REPO"

if origin_url=$(git -C "${repo_root}" remote get-url origin 2>/dev/null); then
    case "${origin_url}" in
        git@github.com:*)
            origin_url="https://github.com/${origin_url#git@github.com:}"
            ;;
        ssh://git@github.com/*)
            origin_url="https://github.com/${origin_url#ssh://git@github.com/}"
            ;;
    esac
    default_pkgurl="${origin_url%.git}"
fi

pkgurl=${VINPUT_PKGURL:-${default_pkgurl}}
source_name=$(basename "${source_archive}")
source_sha256=$(sha256sum "${source_archive}" | awk '{print $1}')

mkdir -p "$(dirname "${output_path}")"

sed \
    -e "s|@VINPUT_PKGVER@|${pkgver}|g" \
    -e "s|@VINPUT_PKGREL@|${pkgrel}|g" \
    -e "s|@VINPUT_PKGURL@|${pkgurl}|g" \
    -e "s|@VINPUT_SOURCE_ARCHIVE@|${source_name}|g" \
    -e "s|@VINPUT_SOURCE_SHA256@|${source_sha256}|g" \
    "${template_path}" > "${output_path}"
