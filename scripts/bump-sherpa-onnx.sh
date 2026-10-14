#!/usr/bin/env bash
# Update the pinned sherpa-onnx version + sha256 digests across the repo.
# Usage: bump-sherpa-onnx.sh <new-version>           # e.g. 1.12.35
#
# Reads the current default version and per-arch sha256 from
# sherpa-onnx-vars.sh, fetches new sha256s from the GitHub API, then
# rewrites:
#   - scripts/sherpa-onnx-vars.sh
#       * default fallback version (`:-X.Y.Z`)
#       * case-branch label `X.Y.Z)`
#       * x86_64 + aarch64 sha256
#   - packaging/flatpak/org.fcitx.Fcitx5.Addon.Vinput.yaml
#   - packaging/flatpak/ci-manifest.yaml
#       * archive URL (contains version twice)
#       * x86_64 sha256
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <new-version>" >&2
  exit 1
fi
new_version="$1"

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/.." && pwd)

# shellcheck source=./sherpa-onnx-vars.sh
source "${script_dir}/sherpa-onnx-vars.sh"

current_version="${SHERPA_ONNX_VERSION}"
if [[ "${current_version}" == "${new_version}" ]]; then
  echo "sherpa-onnx already pinned at v${new_version}, nothing to do." >&2
  exit 0
fi

# Snapshot the OLD per-arch sha256 before any rewrites so we can sed-replace
# the literal hex strings without having to parse the case branch by hand.
sherpa_onnx_set_vars "${current_version}" "x86_64"
old_x64_sha="${SHERPA_ONNX_SHA256}"
sherpa_onnx_set_vars "${current_version}" "aarch64"
old_aarch64_sha="${SHERPA_ONNX_SHA256}"

if [[ -z "${old_x64_sha}" || -z "${old_aarch64_sha}" ]]; then
  echo "Could not read existing sha256 for v${current_version} from sherpa-onnx-vars.sh" >&2
  exit 1
fi

# Resolve NEW per-arch archive names + fetch digests from GitHub API.
sherpa_onnx_set_vars "${new_version}" "x86_64"
new_x64_archive="${SHERPA_ONNX_ARCHIVE}"
echo "Fetching sha256 for ${new_x64_archive}..." >&2
new_x64_sha="$(sherpa_onnx_fetch_digest "${new_version}" "${new_x64_archive}")"
[[ -n "${new_x64_sha}" ]] || {
  echo "GitHub API returned empty x86_64 digest" >&2
  exit 1
}

sherpa_onnx_set_vars "${new_version}" "aarch64"
new_aarch64_archive="${SHERPA_ONNX_ARCHIVE}"
echo "Fetching sha256 for ${new_aarch64_archive}..." >&2
new_aarch64_sha="$(sherpa_onnx_fetch_digest "${new_version}" "${new_aarch64_archive}")"
[[ -n "${new_aarch64_sha}" ]] || {
  echo "GitHub API returned empty aarch64 digest" >&2
  exit 1
}

vars_file="${script_dir}/sherpa-onnx-vars.sh"
flatpak_files=(
  "${repo_root}/packaging/flatpak/org.fcitx.Fcitx5.Addon.Vinput.yaml"
  "${repo_root}/packaging/flatpak/ci-manifest.yaml"
)

sed -i \
  -e "s/:-${current_version}/:-${new_version}/g" \
  -e "s/^\([[:space:]]*\)${current_version})[[:space:]]*$/\1${new_version})/" \
  -e "s/${old_x64_sha}/${new_x64_sha}/g" \
  -e "s/${old_aarch64_sha}/${new_aarch64_sha}/g" \
  "${vars_file}"

for f in "${flatpak_files[@]}"; do
  sed -i \
    -e "s|/v${current_version}/sherpa-onnx-v${current_version}-|/v${new_version}/sherpa-onnx-v${new_version}-|g" \
    -e "s/${old_x64_sha}/${new_x64_sha}/g" \
    "$f"
done

echo "Bumped sherpa-onnx: v${current_version} -> v${new_version}" >&2
