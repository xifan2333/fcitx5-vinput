#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <ppa-ubuntu24.04|copr-fedora43|aur-archlinux> <version> [ppa_revision]" >&2
  exit 1
fi

target=$1
version=$2
ppa_revision=${3:-}

fail_verify() {
  echo "$1" >&2
  exit 1
}

check_no_missing_libs() {
  local binary=$1
  local report

  report=$(mktemp)
  ldd "${binary}" | tee "${report}"
  if grep -Fq "not found" "${report}"; then
    fail_verify "missing shared library dependency detected for ${binary}"
  fi
}

assert_common_install() {
  local addon_path
  local runtime_dir

  test -x /usr/bin/vinput
  test -x /usr/bin/vinput-daemon
  test -x /usr/bin/vinput-gui
  test -f /usr/share/fcitx5/addon/vinput.conf
  test -f /usr/share/systemd/user/vinput-daemon.service
  test -f /usr/share/dbus-1/services/org.fcitx.Vinput.service
  test -f /usr/share/fcitx5-vinput/default-config.json
  test -f /usr/share/man/man1/vinput.1.gz -o -f /usr/share/man/man1/vinput.1

  addon_path=$(find /usr/lib /usr/lib64 -path '*/fcitx5/fcitx5-vinput.so' -print -quit 2>/dev/null || true)
  runtime_dir=$(find /usr/lib /usr/lib64 -path '*/fcitx5-vinput' -type d -print -quit 2>/dev/null || true)

  if [[ -z "${addon_path}" ]]; then
    fail_verify "fcitx5 addon was not installed"
  fi

  if [[ -z "${runtime_dir}" ]]; then
    fail_verify "bundled runtime directory was not installed"
  fi

  test -f "${runtime_dir}/libsherpa-onnx-c-api.so"
  test -f "${runtime_dir}/libsherpa-onnx-cxx-api.so"

  check_no_missing_libs /usr/bin/vinput
  check_no_missing_libs /usr/bin/vinput-daemon
  check_no_missing_libs "${addon_path}"
}

case "${target}" in
ppa-ubuntu24.04)
  export DEBIAN_FRONTEND=noninteractive

  apt-get update
  apt-get install -y --no-install-recommends software-properties-common
  add-apt-repository -y ppa:xifan233/ppa
  apt-get update
  apt-cache policy fcitx5-vinput
  if ! apt-cache show fcitx5-vinput >/dev/null 2>&1; then
    fail_verify "ppa:xifan233/ppa does not currently publish fcitx5-vinput for noble"
  fi
  expected_version=$(apt-cache policy fcitx5-vinput | sed -n 's/  Candidate: //p')
  if [[ -n "${ppa_revision}" ]]; then
    if [[ "${expected_version}" != "${version}-1ppa${ppa_revision}~noble1" ]]; then
      fail_verify "ppa candidate ${expected_version} does not match expected ${version}-1ppa${ppa_revision}~noble1"
    fi
  elif [[ ! "${expected_version}" =~ ^${version}-1ppa[0-9]+~noble1$ ]]; then
    fail_verify "ppa candidate ${expected_version} does not match expected ${version}-1ppaN~noble1 pattern"
  fi
  apt-get install -y --no-install-recommends "fcitx5-vinput=${expected_version}"
  installed_version=$(dpkg-query -W -f='${Version}\n' fcitx5-vinput)
  if [[ "${installed_version}" != "${expected_version}" ]]; then
    fail_verify "ppa installed ${installed_version}, expected ${expected_version}"
  fi
  ;;
copr-fedora43)
  dnf install -y dnf-plugins-core
  dnf copr enable -y xifan/fcitx5-vinput-bin
  dnf install -y fcitx5-vinput
  installed_version=$(rpm -q --qf '%{VERSION}\n' fcitx5-vinput)
  if [[ "${installed_version}" != "${version}" ]]; then
    fail_verify "copr installed ${installed_version}, expected ${version}"
  fi
  ;;
aur-archlinux)
  workspace=/tmp/aur-verify

  pacman -Syu --noconfirm --needed \
    git \
    sudo

  useradd -m builder
  echo "builder ALL=(ALL) NOPASSWD: ALL" >/etc/sudoers.d/builder
  chmod 0440 /etc/sudoers.d/builder
  rm -rf "${workspace}"
  install -d -o builder -g builder "${workspace}"
  su builder -c "git clone --depth 1 https://aur.archlinux.org/yay-bin.git '${workspace}/yay-bin'"
  su builder -c "cd '${workspace}/yay-bin' && makepkg -si --noconfirm"
  aur_version=$(su builder -c "yay -Si fcitx5-vinput-bin | sed -n 's/^Version[[:space:]]*:[[:space:]]*//p' | head -n1 | cut -d- -f1")
  if [[ "${aur_version}" != "${version}" ]]; then
    fail_verify "aur PKGBUILD version is ${aur_version}, expected ${version}"
  fi
  su builder -c "yay -S --noconfirm --answerdiff None --answerclean None fcitx5-vinput-bin"
  installed_version=$(pacman -Q fcitx5-vinput-bin | awk '{print $2}')
  if [[ ! "${installed_version}" =~ ^${version}- ]]; then
    fail_verify "aur installed ${installed_version}, expected ${version}-*"
  fi
  ;;
*)
  fail_verify "unsupported target: ${target}"
  ;;
esac

assert_common_install
