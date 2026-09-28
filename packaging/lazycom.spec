%global upstream_version 0.1.0
%global source_commit unknown
%global package_suffix %{nil}

# The shared serial library belongs to this application only.
%global __provides_exclude_from ^%{_libdir}/lazycom/.*$
%global __requires_exclude ^libserialport[.]so.*$
%global __requires_exclude_from ^%{_datadir}/lazycom/source/.*$

Name:           lazycom
Version:        %{upstream_version}%{package_suffix}
Release:        1%{?dist}
Summary:        Terminal serial port assistant
License:        GPL-3.0-only AND LGPL-3.0-or-later AND MIT AND CC0-1.0
URL:            https://github.com/Augustus-S/lazycom
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc >= 13
BuildRequires:  gcc-c++ >= 13
BuildRequires:  cmake >= 3.28
BuildRequires:  cmake-rpm-macros
BuildRequires:  ninja-build
BuildRequires:  make
BuildRequires:  redhat-rpm-config
BuildRequires:  desktop-file-utils

Provides:       bundled(ftxui) = 6.1.9
Provides:       bundled(libserialport) = 0.1.2
Provides:       bundled(tomlplusplus) = 3.4.0
Provides:       bundled(nlohmann-json) = 3.12.0
Provides:       bundled(tl-expected) = 1.2.0
Provides:       bundled(spdlog) = 1.15.3
Provides:       bundled(fmt) = 11.2.0

%description
LazyCom is a C++20 terminal interface for serial port communication on Linux.
It includes connection management, receive views, configurable sending and
session logging. A private shared libserialport and its source are included.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake \
    -DBUILD_SHARED_LIBS:BOOL=OFF \
    -DCMAKE_BUILD_TYPE:STRING=Release \
    -DCMAKE_INSTALL_LIBDIR:PATH=%{_lib} \
    -DLAZYCOM_BUILD_DIAGNOSTICS:BOOL=ON \
    -DLAZYCOM_USE_SYSTEM_DEPS:BOOL=OFF \
    -DLAZYCOM_USE_SYSTEM_LIBSERIALPORT:BOOL=OFF \
    -DLAZYCOM_WARNINGS_AS_ERRORS:BOOL=OFF \
    -DLAZYCOM_BUILD_COMMIT:STRING=%{source_commit} \
    -DLAZYCOM_PACKAGE_SUFFIX:STRING="%{package_suffix}"
%cmake_build

%install
DESTDIR="%{buildroot}" cmake --install "%{__cmake_builddir}" --component Runtime
mkdir -p "%{buildroot}%{_licensedir}/%{name}" "%{buildroot}%{_docdir}/%{name}"
mv "%{buildroot}%{_datadir}/%{name}/licenses/"* "%{buildroot}%{_licensedir}/%{name}/"
rmdir "%{buildroot}%{_datadir}/%{name}/licenses"
mv "%{buildroot}%{_datadir}/%{name}/Plan.md" \
   "%{buildroot}%{_datadir}/%{name}/releases.md" "%{buildroot}%{_docdir}/%{name}/"
desktop-file-validate "%{buildroot}%{_datadir}/applications/%{name}.desktop"

%files
%license %{_licensedir}/%{name}/
%doc %{_docdir}/%{name}/
%{_bindir}/%{name}
%dir %{_libdir}/%{name}
%{_libdir}/%{name}/libserialport.so*
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/COMMIT
%{_datadir}/%{name}/DEPENDENCIES.lock
%{_datadir}/%{name}/source/
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg

%changelog
* Mon Sep 28 2026 LazyCom contributors - 0.1.0-1
- Add native Fedora packaging with pinned dependencies.
