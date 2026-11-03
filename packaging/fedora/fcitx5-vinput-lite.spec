Name:           fcitx5-vinput-lite
Version:        @VINPUT_VERSION@
Release:        1%{?dist}
Summary:        Voice input addon for Fcitx5 (Lite build)
License:        GPL-3.0-only
URL:            https://github.com/xifan2333/fcitx5-vinput
Source0:        %{url}/archive/v%{version}/fcitx5-vinput-%{version}.tar.gz

Provides:       fcitx5-vinput = %{version}-%{release}
Conflicts:      fcitx5-vinput

BuildRequires:  cmake >= 3.16
BuildRequires:  ninja-build
BuildRequires:  clang
BuildRequires:  mold
BuildRequires:  pkgconfig
BuildRequires:  gettext
BuildRequires:  cmake(Fcitx5Core)
BuildRequires:  cmake(Fcitx5Config)
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
BuildRequires:  spdlog-devel

Requires:       fcitx5
Requires:       pipewire
Requires:       curl
Requires:       systemd
Requires:       qt6-qtbase
Recommends:     wireplumber

%description
Voice input plugin for Fcitx5 with cloud ASR and LLM post-processing (Lite build).

%prep
%autosetup -n fcitx5-vinput-%{version}

%build
export CC=clang
export CXX=clang++
%cmake -G Ninja \
    -DVINPUT_ENABLE_LOCAL_ASR=OFF \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold \
    -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=mold \
    -DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=mold \
    -DVINPUT_PROJECT_VERSION=%{version} \
    -DVINPUT_PACKAGE_RELEASE=%{release} \
    -DVINPUT_PACKAGE_HOMEPAGE_URL=%{url} \
    -DVINPUT_FETCH_SPDLOG=OFF \
    -DVINPUT_FETCH_CLI11=OFF
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_bindir}/vinput
%{_bindir}/vinput-daemon
%{_bindir}/vinput-gui
%{_libdir}/fcitx5/fcitx5-vinput.so
%{_datadir}/fcitx5/addon/vinput.conf
%{_datadir}/dbus-1/services/org.fcitx.Vinput.service
%{_datadir}/systemd/user/vinput-daemon.service
%{_datadir}/locale/*/LC_MESSAGES/fcitx5-vinput.mo
%{_datadir}/fcitx5-vinput/
%{_datadir}/applications/vinput-gui.desktop
%{_datadir}/icons/hicolor/
%{_mandir}/man*/*
%{_mandir}/*/man*/*
%{_datadir}/bash-completion/completions/vinput
%{_datadir}/zsh/site-functions/_vinput
%{_datadir}/fish/vendor_completions.d/vinput.fish

%changelog
* Tue Mar 18 2026 xifan2333 <noreply@github.com> - 0.1.6-1
- Initial RPM package
