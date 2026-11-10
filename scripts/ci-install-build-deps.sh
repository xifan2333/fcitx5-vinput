#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <ubuntu24.04|debian12|fedora43|opensuse-leap|archlinux>" >&2
  exit 1
fi

target=$1
sudo_cmd=()
if [[ ${EUID} -ne 0 ]]; then
  sudo_cmd=(sudo)
fi

# APT writes here instead of /var/cache/apt/archives so actions/cache can save it
# without sudo; a cache hit keeps the archive download off the network entirely.
vinput_apt_archives="${HOME}/.cache/vinput-apt-archives"

readonly qt6_apt_packages=(
  qt6-base-dev
  qt6-tools-dev
  qt6-tools-dev-tools
)

readonly ubuntu_apt_packages=(
  bzip2
  clang
  cmake
  file
  git
  jq
  make
  mold
  ninja-build
  pkg-config
  sccache
  gettext
  curl
  patch
  python3
  zstd
  libblas-dev
  liblapack-dev
  libopenblas-dev
  libssl-dev
  libarchive-dev
  libpipewire-0.3-dev
  libsystemd-dev
  libfcitx5core-dev
  libfcitx5config-dev
  libfcitx5utils-dev
  fcitx5-modules-dev
  "${qt6_apt_packages[@]}"
)

install_qt_apt() {
  "${sudo_cmd[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
    "${qt6_apt_packages[@]}"
}

install_qt_dnf() {
  "${sudo_cmd[@]}" dnf install -y --setopt=install_weak_deps=False \
    qt6-qtbase-devel \
    qt6-qttools-devel
}

install_qt_pacman() {
  pacman -S --noconfirm --needed \
    qt6-base \
    qt6-tools
}

install_qt_zypper() {
  "${sudo_cmd[@]}" zypper --non-interactive install --no-recommends \
    qt6-base-devel \
    qt6-tools-devel
}

apt_opts=(
  -o "Dir::Cache::Archives=${vinput_apt_archives}"
  -o APT::Keep-Downloaded-Packages=true
  -o Acquire::Retries=2
  -o Acquire::http::Timeout=20
  -o Acquire::https::Timeout=20
  -o Acquire::Languages=none
  -o DPkg::Lock::Timeout=300
)

prepare_apt_archives() {
  mkdir -p "${vinput_apt_archives}/partial"
  # APT fetches through the _apt sandbox user; a root-owned 0755 partial/ would
  # make it fall back to an unsandboxed root download.
  if id _apt >/dev/null 2>&1; then
    "${sudo_cmd[@]}" chown _apt:root "${vinput_apt_archives}/partial"
  fi
  "${sudo_cmd[@]}" chmod 700 "${vinput_apt_archives}/partial"
}

# actions/cache runs as the runner user, so hand the directory back before the
# post-step packs it; partial/ is ephemeral and would only bloat the cache.
# Cache hygiene must never decide the build outcome: keep the install status and
# only warn, or a failing chown would mask a real apt failure.
finalize_apt_archives() {
  local status=$?
  "${sudo_cmd[@]}" rm -rf "${vinput_apt_archives}/partial" ||
    echo "warning: could not drop ${vinput_apt_archives}/partial" >&2
  "${sudo_cmd[@]}" chown -R "$(id -u):$(id -g)" "${vinput_apt_archives}" ||
    echo "warning: could not hand ${vinput_apt_archives} back to the runner user" >&2
  return "${status}"
}

# APT never abandons a mirror that trickles bytes, so drop the first (highest
# priority) entry and let the next attempt use the following one.
evict_first_mirror() {
  local mirrors=/etc/apt/apt-mirrors.txt
  [[ -s ${mirrors} ]] || return 0
  echo "evicting $(head -n 1 "${mirrors}") after a download timeout" >&2
  "${sudo_cmd[@]}" sed -i '1d' "${mirrors}"
}

download_ubuntu_archives() {
  timeout "$1" "${sudo_cmd[@]}" env DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --download-only --no-install-recommends \
    "${apt_opts[@]}" "${ubuntu_apt_packages[@]}"
}

# Split download from install: only the download can hang on a slow mirror, and
# only a shell timeout can interrupt a transfer that keeps trickling bytes.
fetch_ubuntu_archives() {
  if download_ubuntu_archives "$1"; then
    return 0
  fi
  evict_first_mirror
  download_ubuntu_archives "$1"
}

update_ubuntu_indices() {
  if timeout "$1" "${sudo_cmd[@]}" apt-get update "${apt_opts[@]}"; then
    return 0
  fi
  evict_first_mirror
  timeout "$1" "${sudo_cmd[@]}" apt-get update "${apt_opts[@]}"
}

install_ubuntu_deps() {
  prepare_apt_archives
  trap finalize_apt_archives EXIT

  update_ubuntu_indices 120
  fetch_ubuntu_archives 240
  # Everything is in the archive dir now, so this install does no network I/O.
  "${sudo_cmd[@]}" env DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends "${apt_opts[@]}" "${ubuntu_apt_packages[@]}"
}

case "${target}" in
ubuntu24.04)
  install_ubuntu_deps
  ;;
debian12)
  apt-get update
  apt-get install -y \
    bzip2 \
    clang-16 \
    cmake \
    make \
    mold \
    ninja-build \
    pkg-config \
    sccache \
    gettext \
    file \
    git \
    curl \
    jq \
    patch \
    python3 \
    zstd \
    libblas-dev \
    liblapack-dev \
    libopenblas-dev \
    libssl-dev \
    libarchive-dev \
    libpipewire-0.3-dev \
    libsystemd-dev \
    libfcitx5core-dev \
    libfcitx5config-dev \
    libfcitx5utils-dev \
    fcitx5-modules-dev
  update-alternatives --install /usr/bin/clang clang /usr/bin/clang-16 100 \
    --slave /usr/bin/clang++ clang++ /usr/bin/clang++-16
  install_qt_apt
  ;;
fedora43)
  # Avoid weak deps that can pull packages from the Cisco openh264 repo,
  # which is intermittently unavailable in GitHub Actions containers.
  "${sudo_cmd[@]}" dnf install -y --setopt=install_weak_deps=False \
    clang \
    cmake \
    mold \
    ninja-build \
    pkgconf-pkg-config \
    sccache \
    gettext \
    curl \
    zstd \
    openssl-devel \
    libarchive-devel \
    pipewire-devel \
    systemd-devel \
    fcitx5-devel
  install_qt_dnf
  ;;
opensuse-leap)
  "${sudo_cmd[@]}" zypper --non-interactive addrepo --refresh \
    https://download.opensuse.org/repositories/devel:/tools:/compiler/15.6/ \
    devel-tools-compiler || true
  "${sudo_cmd[@]}" zypper --gpg-auto-import-keys --non-interactive refresh
  "${sudo_cmd[@]}" zypper --non-interactive install --no-recommends \
    clang17 \
    cmake \
    gettext-tools \
    git \
    jq \
    lapack-devel \
    libarchive-devel \
    libopenblas_pthreads-devel \
    libopenssl-devel \
    make \
    mold \
    ninja \
    openblas-common-devel \
    patch \
    pkg-config \
    pipewire-devel \
    python3 \
    rpm-build \
    sccache \
    systemd-devel \
    zstd \
    fcitx5-devel
  install_qt_zypper
  ;;
archlinux)
  pacman -Syu --noconfirm --needed \
    clang \
    cmake \
    curl \
    fcitx5 \
    git \
    libarchive \
    mold \
    ninja \
    openssl \
    pipewire \
    pkgconf \
    sccache \
    systemd
  install_qt_pacman
  ;;
*)
  echo "unsupported target: ${target}" >&2
  exit 1
  ;;
esac
