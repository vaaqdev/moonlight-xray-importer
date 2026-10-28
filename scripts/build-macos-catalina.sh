#!/bin/bash
# Separate legacy build; never reuse the macOS 13 prebuilt dependencies.
set -euo pipefail
root="$PWD"
work="$root/build/catalina"
prefix="$root/libs/mac"
export MACOSX_DEPLOYMENT_TARGET=10.15
export CFLAGS="-mmacosx-version-min=10.15 -arch x86_64"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="$CFLAGS"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
export CMAKE_PREFIX_PATH="$prefix"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
jobs="$(sysctl -n hw.logicalcpu)"
mkdir -p "$work" "$prefix"
test "$(uname -m)" = x86_64

# v19 pins the source revisions, NOT the incompatible prebuilt binaries.
git clone --depth 1 --branch v19 https://github.com/moonlight-stream/moonlight-qt-deps.git "$work/deps"
git -C "$work/deps" submodule update --init --recursive --depth 1 openssl opus SDL_ttf FFmpeg dav1d
git clone --depth 1 --branch release-2.32.10 https://github.com/libsdl-org/SDL.git "$work/SDL2"

cmake_build() {
  local source="$1" destination="$2"
  shift 2
  cmake -S "$source" -B "$destination" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_NAME_DIR=@rpath \
    -DCMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON "$@"
  cmake --build "$destination" --parallel "$jobs"
  cmake --install "$destination"
}
cmake_build "$work/SDL2" "$work/sdl-build" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_HIDAPI_LIBUSB=OFF
cmake_build "$work/deps/SDL_ttf" "$work/ttf-build" \
  -DBUILD_SHARED_LIBS=ON -DSDL2TTF_VENDORED=ON -DSDL2TTF_SAMPLES=OFF
cmake_build "$work/deps/opus" "$work/opus-build" \
  -DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF

pushd "$work/deps/openssl"
./Configure darwin64-x86_64-cc --prefix="$prefix" --openssldir=/etc/ssl \
  -mmacosx-version-min=10.15 shared no-tests no-apps no-legacy no-engine
make -j"$jobs"
make install_sw
popd

meson setup "$work/dav1d-build" "$work/deps/dav1d" --prefix="$prefix" \
  --libdir=lib --buildtype=release -Ddefault_library=shared \
  -Denable_tools=false -Denable_tests=false
meson compile -C "$work/dav1d-build" -j "$jobs"
meson install -C "$work/dav1d-build"

mkdir -p "$work/ffmpeg-build"
pushd "$work/ffmpeg-build"
"$work/deps/FFmpeg/configure" --prefix="$prefix" --cc=clang --arch=x86_64 \
  --extra-cflags="$CFLAGS -I$prefix/include" --extra-ldflags="$LDFLAGS -L$prefix/lib" \
  --install-name-dir=@rpath --enable-shared --disable-static --enable-pic \
  --disable-all --disable-autodetect --enable-avcodec --enable-swscale \
  --enable-decoder=h264,hevc,av1,libdav1d --enable-libdav1d --enable-videotoolbox \
  --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox,av1_videotoolbox
make -j"$jobs"
make install
popd

# Normalize every native install name, including transitive dependencies.
python3 "$root/scripts/relocate-catalina-libs.py" "$prefix/lib"
mkdir -p "$work/app-build"
pushd "$work/app-build"
qmake "$root/moonlight-qt.pro" CONFIG+=catalina QMAKE_APPLE_DEVICE_ARCHS=x86_64 QMAKE_MACOSX_DEPLOYMENT_TARGET=10.15
make -j"$jobs" release
popd
app="$work/app-build/app/Moonlight.app"
/usr/libexec/PlistBuddy -c 'Set :LSMinimumSystemVersion 10.15' "$app/Contents/Info.plist"
macdeployqt "$app" -qmldir="$root/app/gui" -appstore-compliant
rm -rf "$app/Contents/PlugIns/sqldrivers"

# Go 1.23 is the last Go release supporting Catalina. Newer official Xray
# downloads require a newer macOS even if their Mach-O minimum is patched.
git clone --depth 1 --branch v25.1.30 https://github.com/XTLS/Xray-core.git "$work/xray-source"
xray="$app/Contents/Resources/Xray"
mkdir -p "$xray"
pushd "$work/xray-source"
CGO_ENABLED=0 GOOS=darwin GOARCH=amd64 GOAMD64=v1 \
  go build -trimpath -ldflags='-s -w -buildid=' -o "$xray/xray-x86_64" ./main
popd
cp "$work/xray-source/LICENSE" "$xray/LICENSE"
curl -fL --retry 3 https://github.com/XTLS/Xray-core/releases/download/v25.1.30/Xray-macos-64.zip -o "$work/xray-data.zip"
unzip -j "$work/xray-data.zip" geoip.dat geosite.dat -d "$xray"
cc $CFLAGS -O2 -Wall -Werror "$root/scripts/xray-launcher.c" -o "$xray/xray-launcher"
cp "$root/LICENSE" "$app/Contents/Resources/LICENSE-Moonlight.txt"
cp "$root/scripts/README-catalina.txt" "$app/Contents/Resources/README-catalina.txt"
codesign --force --sign - "$xray/xray-x86_64"
codesign --force --sign - "$xray/xray-launcher"
codesign --force --deep --sign - "$app"
python3 scripts/verify-macos-bundle.py "$app" --catalina

mkdir -p "$work/tunnel-tests"
pushd "$work/tunnel-tests"
qmake "$root/scripts/test-integrated-tunnel.pro" CONFIG+=release QMAKE_MACOSX_DEPLOYMENT_TARGET=10.15
make -j"$jobs"
cp -R "$xray" ./Xray
./tunnel-manager-tests --import-tests
./tunnel-manager-tests --local-lifecycle-tests
./tunnel-manager-tests --diagnostics-tests
popd

name=Moonlight-6.2.0-macOS-10.15-Catalina-Portable-Intel
stage="$work/$name"
mkdir -p "$stage" "$root/build/catalina-portable"
ditto "$app" "$stage/Moonlight.app"
cp "$root/scripts/README-catalina.txt" "$stage/LEEME.txt"
output="$root/build/catalina-portable/$name.zip"
ditto -c -k --sequesterRsrc --keepParent "$stage" "$output"
mkdir -p "$work/extract-test"
ditto -x -k "$output" "$work/extract-test"
python3 scripts/verify-macos-bundle.py "$work/extract-test/$name/Moonlight.app" --catalina
shasum -a 256 "$output" > "$output.sha256"
