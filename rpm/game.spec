%define _app_orgname ru.sashikknox
%define _app_appname luanti
%define _app_launcher_name Luanti
%define _ssl_version 3.0.15

Name:       %{_app_orgname}.%{_app_appname}
Summary:    Luanti Game Engine for AuroraOS
Release:    1
Version:    5.14.0
Group:      Amusements/Games
License:    GPL3
Source0:    %{name}.tar.gz

%define __requires_exclude ^libicu.*\\.so.*|libsqlite3\\.so.*|libatomic\\.so.*|libcrypto*\\.so.*|libopenal\\.so.*|libjpeg\\.so.*|libfreetype\\.so.*|libncursesw\\.so\\.6|libzstd\\.so.*|libtinfo\\.so.*$
%define __provides_exclude_from ^%{_datadir}/%{name}/lib/.*\\.so.*$

BuildRequires: pkgconfig(openal)
BuildRequires: perl-IPC-Cmd
BuildRequires: libatomic
BuildRequires: pkgconfig(libcurl)
BuildRequires: pkgconfig(libjpeg)
BuildRequires: pkgconfig(sqlite3)
BuildRequires: pkgconfig(libzstd)
BuildRequires: pkgconfig(harfbuzz)
BuildRequires: pkgconfig(theoradec)
BuildRequires: pkgconfig(vorbis)
BuildRequires: pkgconfig(zlib)
BuildRequires: pkgconfig(freetype2)
BuildRequires: pkgconfig(libmpg123)
BuildRequires: pkgconfig(wayland-client)
BuildRequires: pkgconfig(wayland-cursor)
BuildRequires: pkgconfig(wayland-egl)
BuildRequires: pkgconfig(wayland-protocols)
BuildRequires: pkgconfig(wayland-scanner)
BuildRequires: pkgconfig(glesv2)
BuildRequires: pkgconfig(xkbcommon)
BuildRequires: pkgconfig(vulkan)
BuildRequires: pkgconfig(egl)
BuildRequires: pkgconfig(sdl2)
BuildRequires: rsync
BuildRequires: patchelf
BuildRequires: zip
BuildRequires: ninja
BuildRequires: lua

%description
%{summary}

%prep
%setup -q -n %{name}-%{version}
mkdir -p build/%{_arch}
# clone OpenSSL to build dir
echo "clone OpenSSL to build dir"
rsync -zavP lib/openssl-%{_ssl_version} build/%{_arch}/
# clone LuaJIT to build dir
echo "clone LuaJIT to build dir"
rsync -aP LuaJIT build/%{_arch}/


%build
# SDL2 build
cmake \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -Bbuild/%{_arch}/libsdl \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_PULSEAUDIO=OFF \
    -DSDL_RPATH=OFF \
    -DSDL_AUDIO=OFF \
    -DSDL_STATIC=OFF \
    -DSDL_WAYLAND=ON \
    -DSDL_X11=OFF \
    -DSDL_WAYLAND_LIBDECOR=OFF \
    libsdl

cmake --build build/%{_arch}/libsdl -j`nproc`

# OpenSSL build
pushd build/%{_arch}/openssl-%{_ssl_version}
    ./Configure --prefix `pwd`/install --openssldir=`pwd`/install
    make -j`nproc` build_generated
    make -j`nproc` build_libs_nodep
    make DESTDIR=`pwd`/install install_dev
popd

pushd build/%{_arch}/LuaJIT
    make CFLAGS="-fPIC" -j`nproc`
    make DESTDIR="`pwd`/install" install
popd

sslpath="`pwd`/build/%{_arch}/openssl-%{_ssl_version}/install/usr/local/"
cmake \
    -G Ninja \
    -B build/%{_arch}/luanti \
    -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPENGL=NO \
    -DENABLE_OPENGL3=NO \
    -DENABLE_GLES2=YES \
    -DENABLE_OPENSSL=YES \
    -DENABLE_LTO=NO \
    -DAURORAOS=YES \
    -DAURORA_DATA_DIR="%{_app_orgname}/%{_app_appname}" \
    -DPROJECT_NAME=%{name} \
    -DOPENSSL_ROOT_DIR=${sslpath} \
    -DOPENSSL_INCLUDE_DIR=${sslpath}/include \
    -DBUILD_DOCUMENTATION=NO \
    -DOPENGL_INCLUDE_DIR="" \
    -DBUILD_CLIENT=YES \
    -DBUILD_SERVER=NO \
    -DCUSTOM_SHAREDIR=/usr/share/%{name}/engine \
    -DUSE_LUAJIT=YES \
    -DLUA_INCLUDE_DIR="`pwd`/build/%{_arch}/LuaJIT/install/usr/local/include/luajit-2.1" \
    -DLUA_LIBRARY="`pwd`/build/%{_arch}/LuaJIT/install/usr/local/lib/libluajit-5.1.a" \
    -S `pwd`

cmake --build build/%{_arch}/luanti -j`nproc`
env DESTDIR=build/%{_arch}/luanti/install  cmake --install build/%{_arch}/luanti


%install
# grab dependencies from target 
install -D %{_libdir}/libicu*.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libsqlite3.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libzstd.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libncursesw.so.6 -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libjpeg.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libopenal.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libatomic.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libfreetype.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D %{_libdir}/libtinfo.so* -t %{buildroot}%{_datadir}/%{name}/lib
# install other dependecies
%ifarch x86_64
    ssl_path=build/%{_arch}/openssl-%{_ssl_version}/install/usr/local/lib64
%else
    ssl_path=build/%{_arch}/openssl-%{_ssl_version}/install/usr/local/lib
%endif
install -D -s ${ssl_path}/libssl.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D -s ${ssl_path}/libcrypto.so* -t %{buildroot}%{_datadir}/%{name}/lib
install -D -s build/%{_arch}/libsdl/libSDL2-2.0.so* -t %{buildroot}%{_datadir}/%{name}/lib
mv build/%{_arch}/luanti/bin/luanti build/%{_arch}/luanti/bin/luanti_patched
patchelf --force-rpath --set-rpath %{_datadir}/%{name}/lib build/%{_arch}/luanti/bin/luanti_patched
install -D -s build/%{_arch}/luanti/bin/luanti_patched  %{buildroot}%{_bindir}/%{name}
install -m 655 -D aurora/icons/86.png  %{buildroot}%{_datadir}/icons/hicolor/86x86/apps/%{name}.png
install -m 655 -D aurora/icons/108.png %{buildroot}%{_datadir}/icons/hicolor/108x108/apps/%{name}.png
install -m 655 -D aurora/icons/128.png %{buildroot}%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
install -m 655 -D aurora/icons/172.png %{buildroot}%{_datadir}/icons/hicolor/172x172/apps/%{name}.png
install -m 655 -D aurora/luanti.desktop %{buildroot}%{_datadir}/applications/%{name}.desktop
rsync -aP build/%{_arch}/luanti/install/usr/share/%{name}/engine %{buildroot}%{_datadir}/%{name}/

%files
%defattr(-,root,root,-)
%attr(755,root,root) %{_bindir}/%{name}
%{_datadir}/icons/hicolor/86x86/apps/%{name}.png
%{_datadir}/icons/hicolor/108x108/apps/%{name}.png
%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
%{_datadir}/icons/hicolor/172x172/apps/%{name}.png
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
