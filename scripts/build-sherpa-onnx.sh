#!/usr/bin/env bash
# Install pre-built sherpa-onnx shared libraries from a local archive or
# from the upstream release after checksum verification (via GitHub API digest).
# Usage: build-sherpa-onnx.sh [version] [prefix] [archive_path]
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=./sherpa-onnx-vars.sh
source "${script_dir}/sherpa-onnx-vars.sh"

version="${1:-${SHERPA_ONNX_VERSION}}"
prefix="${2:-/usr}"
archive_path="${3:-}"

sherpa_onnx_set_vars "${version}"

workdir="$(mktemp -d)"
trap 'rm -rf "${workdir}"' EXIT

if [[ -z "${archive_path}" ]]; then
  archive_path="${workdir}/${SHERPA_ONNX_ARCHIVE}"
  curl -fL --retry 3 --retry-delay 2 -o "${archive_path}" "${SHERPA_ONNX_URL}"
fi

expected_sha256="${SHERPA_ONNX_SHA256:-}"
if [[ -z "${expected_sha256}" ]]; then
  echo "Fetching digest from GitHub API..." >&2
  expected_sha256="$(sherpa_onnx_fetch_digest "${version}" "${SHERPA_ONNX_ARCHIVE}")" || {
    echo "Failed to fetch digest for ${SHERPA_ONNX_ARCHIVE} from GitHub API." >&2
    exit 1
  }
fi

actual_sha256="$(sha256sum "${archive_path}" | awk '{print $1}')"
if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
  echo "Checksum mismatch for ${archive_path}: expected ${expected_sha256}, got ${actual_sha256}" >&2
  exit 1
fi

tar -xjf "${archive_path}" -C "${workdir}"

# 修复 aarch64 包缺失头文件的问题
if [[ "$(uname -m)" == "aarch64" && ! -d "${workdir}/${SHERPA_ONNX_STRIP_DIR}/include" ]]; then
  echo "aarch64 package missing include directory, fetching headers from x64 package..." >&2
  # Save current vars
  main_archive="${SHERPA_ONNX_ARCHIVE}"
  main_strip_dir="${SHERPA_ONNX_STRIP_DIR}"

  # Get x64 vars
  sherpa_onnx_set_vars "${version}" "x86_64"
  x64_archive="${SHERPA_ONNX_ARCHIVE}"
  x64_url="${SHERPA_ONNX_URL}"
  x64_strip_dir="${SHERPA_ONNX_STRIP_DIR}"

  curl -fL --retry 3 -o "${workdir}/${x64_archive}" "${x64_url}"
  tar -xjf "${workdir}/${x64_archive}" -C "${workdir}" "${x64_strip_dir}/include"
  mv "${workdir}/${x64_strip_dir}/include" "${workdir}/${main_strip_dir}/"

  # Restore main vars (optional but good practice if more steps follow)
  sherpa_onnx_set_vars "${version}"
fi

install -d "${prefix}/lib" "${prefix}/include/sherpa-onnx/c-api"
install -m 755 "${workdir}/${SHERPA_ONNX_STRIP_DIR}/lib/"*.so "${prefix}/lib/"
install -m 644 "${workdir}/${SHERPA_ONNX_STRIP_DIR}/include/sherpa-onnx/c-api/"*.h "${prefix}/include/sherpa-onnx/c-api/"
