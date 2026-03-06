#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 <output.tar.gz> <version> [repo-root]" >&2
    exit 1
fi

output_path=$1
version=$2
repo_root=${3:-$(pwd)}
archive_root="fcitx5-vinput-${version}"

mkdir -p "$(dirname "${output_path}")"

if git -C "${repo_root}" rev-parse --verify HEAD >/dev/null 2>&1; then
    git -C "${repo_root}" archive \
        --format=tar.gz \
        --prefix="${archive_root}/" \
        -o "${output_path}" \
        HEAD
    exit 0
fi

tar \
    --exclude-vcs \
    --exclude='.git' \
    --exclude='build' \
    --exclude='build-*' \
    --exclude='dist' \
    --exclude='packaging/arch/pkg' \
    --exclude='packaging/arch/src' \
    --exclude='packaging/arch/*.pkg.tar.zst' \
    --exclude='packaging/arch/*.tar.gz' \
    --exclude='packaging/arch/PKGBUILD' \
    --transform="s,^\.$,${archive_root}," \
    --transform="s,^\./,${archive_root}/," \
    -czf "${output_path}" \
    -C "${repo_root}" \
    .
