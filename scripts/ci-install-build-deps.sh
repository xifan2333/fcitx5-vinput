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

install_qt_apt() {
  "${sudo_cmd[@]}" apt-get install -y \
    qt6-base-dev \
    qt6-tools-dev \
    qt6-tools-dev-tools
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

case "${target}" in
ubuntu24.04)
  "${sudo_cmd[@]}" apt-get update
  "${sudo_cmd[@]}" apt-get install -y \
    bzip2 \
    clang \
    cmake \
    file \
    git \
    jq \
    make \
    mold \
    ninja-build \
    pkg-config \
    sccache \
    gettext \
    curl \
    patch \
    python3 \
    zstd \
    libblas-dev \
    libcurl4-openssl-dev \
    liblapack-dev \
    libopenblas-dev \
    libssl-dev \
    libarchive-dev \
    libpipewire-0.3-dev \
    libsystemd-dev \
    libfcitx5core-dev \
    libfcitx5config-dev \
    libfcitx5utils-dev \
    fcitx5-modules-dev \
    libcli11-dev \
    libspdlog-dev \
    nlohmann-json3-dev
  install_qt_apt
  ;;
debian12)
  apt-get update
  apt-get install -y \
    bzip2 \
    clang \
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
    libcurl4-openssl-dev \
    liblapack-dev \
    libopenblas-dev \
    libssl-dev \
    libarchive-dev \
    libpipewire-0.3-dev \
    libsystemd-dev \
    libfcitx5core-dev \
    libfcitx5config-dev \
    libfcitx5utils-dev \
    fcitx5-modules-dev \
    libcli11-dev \
    libspdlog-dev \
    nlohmann-json3-dev
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
    libcurl-devel \
    openssl-devel \
    libarchive-devel \
    pipewire-devel \
    systemd-devel \
    fcitx5-devel \
    nlohmann-json-devel \
    cli11-devel \
    spdlog-devel
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
    libcurl-devel \
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
    fcitx5-devel \
    cli11-devel \
    nlohmann_json-devel \
    spdlog-devel
  install_qt_zypper
  ;;
archlinux)
  pacman -Syu --noconfirm --needed \
    cli11 \
    clang \
    cmake \
    curl \
    fcitx5 \
    git \
    libarchive \
    mold \
    ninja \
    nlohmann-json \
    openssl \
    pipewire \
    pkgconf \
    sccache \
    spdlog \
    systemd
  install_qt_pacman
  ;;
*)
  echo "unsupported target: ${target}" >&2
  exit 1
  ;;
esac
