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
  "${sudo_cmd[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
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

configure_ubuntu_mirror() {
  local candidates=()

  # 1. 本地优先：读取 Worker/Runner 磁盘预置镜像列表 (0 网络开销)
  if [[ -f /etc/apt/apt-mirrors.txt ]]; then
    while IFS= read -r line; do
      local url
      url=$(echo "${line}" | awk '{print $1}')
      [[ "${url}" =~ ^https?:// ]] && candidates+=("${url%/}")
    done </etc/apt/apt-mirrors.txt
  fi

  # 2. 内置高可用静态候选池（全本地定义，无任何网络依赖）
  local built_in_mirrors=(
    "http://azure.archive.ubuntu.com/ubuntu"
    "http://mirrors.edge.kernel.org/ubuntu"
    "http://archive.ubuntu.com/ubuntu"
  )
  for m in "${built_in_mirrors[@]}"; do
    candidates+=("${m}")
  done

  # 3. 本地去重后，使用严格短超时探测（连接 1 秒，总限时 2 秒）
  local tested=()
  local best_mirror=""
  for m in "${candidates[@]}"; do
    [[ " ${tested[*]:-} " =~ " ${m} " ]] && continue
    tested+=("${m}")

    if curl -s -f --connect-timeout 1 --max-time 2 -I "${m}/dists/noble/InRelease" >/dev/null 2>&1; then
      best_mirror="${m}"
      break
    fi
  done

  if [[ -z "${best_mirror}" ]]; then
    best_mirror="http://azure.archive.ubuntu.com/ubuntu"
  fi

  echo "Selected Ubuntu mirror: ${best_mirror}" >&2

  # 4. 替换当前 sources 配置为选定的健康镜像
  "${sudo_cmd[@]}" sed -i -E "s|https?://[^/]+/ubuntu/?|${best_mirror}/|g" \
    /etc/apt/sources.list /etc/apt/sources.list.d/*.sources 2>/dev/null || true

  # 5. 注入短路快速失败配置，杜绝无意义等待
  "${sudo_cmd[@]}" tee /etc/apt/apt.conf.d/99-ci-fail-fast >/dev/null <<'EOF'
Acquire::Retries "1";
Acquire::http::Timeout "15";
Acquire::https::Timeout "15";
EOF
}

case "${target}" in
ubuntu24.04)
  configure_ubuntu_mirror
  "${sudo_cmd[@]}" apt-get update
  "${sudo_cmd[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y \
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
  install_qt_apt
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
