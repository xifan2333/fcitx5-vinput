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

# archive.ubuntu.com measured 5.8-7.7 MB/s on GitHub-hosted runners; the Azure
# and kernel.org mirrors are fallbacks for when it fluctuates.
readonly UBUNTU_MIRRORS=(
  "http://archive.ubuntu.com/ubuntu"
  "http://azure.archive.ubuntu.com/ubuntu"
  "http://mirrors.edge.kernel.org/ubuntu"
)

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

configure_ubuntu_mirror() {
  local mirror=$1
  local file

  # Ubuntu 24.04 routes every pocket through /etc/apt/apt-mirrors.txt
  # (URIs: mirror+file:), so the list itself is the switch, not sources.list.
  if [[ -f /etc/apt/apt-mirrors.txt ]]; then
    printf '%s\n' "${mirror}" | "${sudo_cmd[@]}" tee /etc/apt/apt-mirrors.txt >/dev/null
  fi

  # Only rewrite stanzas that already point at an Ubuntu archive, so the
  # runner's packages.microsoft.com repo keeps working.
  for file in /etc/apt/sources.list /etc/apt/sources.list.d/*.sources; do
    [[ -f ${file} ]] || continue
    "${sudo_cmd[@]}" sed -i -E \
      -e "s|^(URIs:)[[:space:]]*https?://(azure[.]archive[.]ubuntu[.]com\|archive[.]ubuntu[.]com\|security[.]ubuntu[.]com\|mirrors[.]edge[.]kernel[.]org\|ports[.]ubuntu[.]com)/ubuntu.*|\1 ${mirror}/|" \
      -e "s|^(deb[[:space:]]+)https?://(azure[.]archive[.]ubuntu[.]com\|archive[.]ubuntu[.]com\|security[.]ubuntu[.]com\|mirrors[.]edge[.]kernel[.]org\|ports[.]ubuntu[.]com)/ubuntu|\1${mirror}|" \
      "${file}"
  done
}

install_ubuntu_deps() {
  local mirror
  local -a acquire=(
    -o Acquire::Retries=2
    -o Acquire::http::Timeout=20
    -o Acquire::https::Timeout=20
  )

  for mirror in "${UBUNTU_MIRRORS[@]}"; do
    echo "Ubuntu mirror: ${mirror}" >&2
    configure_ubuntu_mirror "${mirror}"
    # Hard wall-clock caps turn a throttled mirror into a fast retry instead of
    # a 15-minute job timeout; the ladder only advances on real failure.
    if timeout 180 "${sudo_cmd[@]}" apt-get update "${acquire[@]}" &&
      timeout 360 "${sudo_cmd[@]}" env DEBIAN_FRONTEND=noninteractive \
        apt-get install -y "${acquire[@]}" "${ubuntu_apt_packages[@]}"; then
      return 0
    fi
    echo "Ubuntu mirror ${mirror} failed or timed out, falling back" >&2
  done

  echo "all Ubuntu mirrors failed" >&2
  return 1
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
