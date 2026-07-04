%global sherpa_onnx_ver @SHERPA_ONNX_VERSION@
%global __provides_exclude_from ^%{_libdir}/fcitx5-vinput/.*$
%global __requires_exclude_from ^%{_libdir}/fcitx5-vinput/.*$
%global __requires_exclude ^lib(onnxruntime|sherpa-onnx-c-api|sherpa-onnx-cxx-api)\\.so(\\(.*\\))?$

Name:           fcitx5-vinput
Version:        @VINPUT_VERSION@
Release:        1%{?dist}
Summary:        Offline voice input addon for Fcitx5
License:        GPL-3.0-only
URL:            https://github.com/xifan2333/fcitx5-vinput
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz
Source1:        sherpa-onnx-v%{sherpa_onnx_ver}-linux-x64-shared-no-tts.tar.bz2

BuildRequires:  cmake >= 3.16
BuildRequires:  ninja-build
BuildRequires:  clang
BuildRequires:  mold
BuildRequires:  pkgconfig
BuildRequires:  gettext
BuildRequires:  cmake(Fcitx5Core)
BuildRequires:  cmake(Fcitx5Config)
BuildRequires:  cmake(nlohmann_json) >= 3.2.0
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6LinguistTools)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(libarchive)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  cli11-devel

Requires:       fcitx5
Requires:       pipewire
Requires:       curl
Requires:       systemd
Requires:       qt6-qtbase
Recommends:     wireplumber

# Bundled sherpa-onnx/onnxruntime shared libraries are private runtime
# dependencies installed under %{_libdir}/fcitx5-vinput/. They should not
# participate in RPM auto provides/requires, otherwise the package may end up
# depending on the exact onnxruntime symbol version from the build root.

%description
Local offline voice input plugin for Fcitx5, powered by sherpa-onnx
for on-device speech recognition with optional LLM post-processing
via any OpenAI-compatible API.

%prep
%autosetup -n %{name}-%{version}

%build
export CC=clang
export CXX=clang++
bash scripts/build-sherpa-onnx.sh %{sherpa_onnx_ver} %{_builddir}/sherpa-onnx-install %{SOURCE1}
%cmake -G Ninja \
    -DCMAKE_PREFIX_PATH=%{_builddir}/sherpa-onnx-install \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold \
    -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold \
    -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=mold \
    -DVINPUT_PROJECT_VERSION=%{version} \
    -DVINPUT_PACKAGE_RELEASE=%{release} \
    -DVINPUT_PACKAGE_HOMEPAGE_URL=%{url}
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_bindir}/vinput
%{_bindir}/vinput-daemon
%{_bindir}/vinput-gui
%{_libdir}/fcitx5/fcitx5-vinput.so
%{_libdir}/fcitx5-vinput/
%{_datadir}/fcitx5/addon/vinput.conf
%{_datadir}/dbus-1/services/org.fcitx.Vinput.service
%{_datadir}/systemd/user/vinput-daemon.service
%{_datadir}/locale/*/LC_MESSAGES/fcitx5-vinput.mo
%{_datadir}/fcitx5-vinput/
%{_datadir}/applications/vinput-gui.desktop
%{_datadir}/icons/hicolor/

%changelog
* Tue Mar 18 2026 xifan2333 <noreply@github.com> - 0.1.6-1
- Initial RPM package
