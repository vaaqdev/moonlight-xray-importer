# This script requires create-dmg to be installed from https://github.com/sindresorhus/create-dmg
BUILD_CONFIG=$1

fail()
{
	echo "$1" 1>&2
	exit 1
}

if [ "$BUILD_CONFIG" != "Debug" ] && [ "$BUILD_CONFIG" != "Release" ]; then
  fail "Invalid build configuration - expected 'Debug' or 'Release'"
fi

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

if [ -n "$CI_VERSION" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
fi

if [ "$SIGNING_PROVIDER_SHORTNAME" == "" ]; then
  SIGNING_PROVIDER_SHORTNAME=$SIGNING_IDENTITY
fi
if [ "$SIGNING_IDENTITY" == "" ]; then
  SIGNING_IDENTITY=$SIGNING_PROVIDER_SHORTNAME
fi

[ "$SIGNING_IDENTITY" == "" ] || git diff-index --quiet HEAD -- || fail "Signed release builds must not have unstaged changes!"

echo Updating dependencies
python3 $SOURCE_ROOT/setup-deps.py

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir -p "$BUILD_FOLDER" "$INSTALLER_FOLDER" || fail "Cannot create build directories!"

# Enable LTO for official builds
export CFLAGS=-flto=thin
export CXXFLAGS=-flto=thin
export LDFLAGS=-flto=thin

echo Configuring the project
pushd $BUILD_FOLDER
qmake $SOURCE_ROOT/moonlight-qt.pro QMAKE_APPLE_DEVICE_ARCHS="x86_64 arm64" || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
make -j$(sysctl -n hw.logicalcpu) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

echo Saving dSYM file
pushd $BUILD_FOLDER
dsymutil app/Moonlight.app/Contents/MacOS/Moonlight -o Moonlight-$VERSION.dsym || fail "dSYM creation failed!"
cp -R Moonlight-$VERSION.dsym $INSTALLER_FOLDER || fail "dSYM copy failed!"
popd

echo Creating app bundle
EXTRA_ARGS=
if [ "$BUILD_CONFIG" == "Debug" ]; then EXTRA_ARGS="$EXTRA_ARGS -use-debug-libs"; fi
echo Extra deployment arguments: $EXTRA_ARGS
macdeployqt $BUILD_FOLDER/app/Moonlight.app $EXTRA_ARGS -qmldir=$SOURCE_ROOT/app/gui -appstore-compliant || fail "macdeployqt failed!"

echo Removing dSYM files from app bundle
find $BUILD_FOLDER/app/Moonlight.app/ -name '*.dSYM' | xargs rm -rf

echo Bundling Xray tunnel core
XRAY_RESOURCES=$BUILD_FOLDER/app/Moonlight.app/Contents/Resources/Xray
mkdir -p "$XRAY_RESOURCES" || fail "Cannot create Xray resources directory!"
XRAY_CACHE=$SOURCE_ROOT/libs/mac/xray
XRAY_VERSION=${XRAY_VERSION:-v26.3.27}
if [ ! -f "$XRAY_CACHE/xray" ] || [ ! -f "$XRAY_CACHE/xray-x86_64" ]; then
  echo Downloading Xray-core $XRAY_VERSION
  XRAY_WORK=`mktemp -d`
  curl -fsSL "https://github.com/XTLS/Xray-core/releases/download/$XRAY_VERSION/Xray-macos-arm64-v8a.zip" -o "$XRAY_WORK/arm64.zip" || fail "Xray arm64 download failed!"
  curl -fsSL "https://github.com/XTLS/Xray-core/releases/download/$XRAY_VERSION/Xray-macos-64.zip" -o "$XRAY_WORK/x64.zip" || fail "Xray x86_64 download failed!"
  mkdir -p "$XRAY_CACHE" || fail "Cannot create Xray cache directory!"
  unzip -o -q "$XRAY_WORK/arm64.zip" -d "$XRAY_WORK/arm64" || fail "Xray arm64 unzip failed!"
  unzip -o -q "$XRAY_WORK/x64.zip" -d "$XRAY_WORK/x64" || fail "Xray x86_64 unzip failed!"
  cp "$XRAY_WORK/arm64/xray" "$XRAY_CACHE/xray" || fail "Xray arm64 extract failed!"
  cp "$XRAY_WORK/x64/xray" "$XRAY_CACHE/xray-x86_64" || fail "Xray x86_64 extract failed!"
  cp "$XRAY_WORK/arm64/LICENSE" "$XRAY_CACHE/LICENSE" || fail "Xray license extract failed!"
  for data_file in geoip.dat geosite.dat; do
    if [ -f "$XRAY_WORK/arm64/$data_file" ]; then cp "$XRAY_WORK/arm64/$data_file" "$XRAY_CACHE/$data_file"; fi
  done
  rm -rf "$XRAY_WORK"
fi
cp "$XRAY_CACHE/xray" "$XRAY_RESOURCES/xray" || fail "Xray arm64 copy failed!"
cp "$XRAY_CACHE/xray-x86_64" "$XRAY_RESOURCES/xray-x86_64" || fail "Xray x86_64 copy failed!"
for data_file in geoip.dat geosite.dat; do
  if [ -f "$XRAY_CACHE/$data_file" ]; then cp "$XRAY_CACHE/$data_file" "$XRAY_RESOURCES/$data_file"; fi
done
chmod +x "$XRAY_RESOURCES/xray" "$XRAY_RESOURCES/xray-x86_64" || fail "chmod Xray failed!"
cc -O2 -Wall -Werror -arch x86_64 -arch arm64 -o "$XRAY_RESOURCES/xray-launcher" "$SOURCE_ROOT/scripts/xray-launcher.c" || fail "xray-launcher build failed!"
echo Xray tunnel core bundled
cp "$XRAY_CACHE/LICENSE" "$XRAY_RESOURCES/LICENSE" || fail "Xray license copy failed!"
cp "$SOURCE_ROOT/LICENSE" "$BUILD_FOLDER/app/Moonlight.app/Contents/Resources/LICENSE-Moonlight.txt" || fail "Moonlight license copy failed!"

if [ "$SIGNING_IDENTITY" != "" ]; then
  echo Signing app bundle
  codesign --force --deep --options runtime --timestamp --sign "$SIGNING_IDENTITY" $BUILD_FOLDER/app/Moonlight.app || fail "Signing failed!"
else
  # Apple Silicon requires valid code signatures. Ad-hoc signing permits local
  # execution; Developer ID signing/notarization remains a separate release step.
  codesign --force --sign - "$XRAY_RESOURCES/xray" || fail "Xray arm64 ad-hoc signing failed!"
  codesign --force --sign - "$XRAY_RESOURCES/xray-x86_64" || fail "Xray x86_64 ad-hoc signing failed!"
  codesign --force --sign - "$XRAY_RESOURCES/xray-launcher" || fail "Launcher ad-hoc signing failed!"
  codesign --force --deep --sign - "$BUILD_FOLDER/app/Moonlight.app" || fail "Bundle ad-hoc signing failed!"
fi

echo Creating DMG
if [ "$SIGNING_IDENTITY" != "" ]; then
  create-dmg $BUILD_FOLDER/app/Moonlight.app $INSTALLER_FOLDER --identity="$SIGNING_IDENTITY" --no-version-in-filename || fail "create-dmg failed!"
else
  create-dmg $BUILD_FOLDER/app/Moonlight.app $INSTALLER_FOLDER --no-version-in-filename
  case $? in
    0) ;;
    2) ;;
    *) fail "create-dmg failed!";;
  esac
fi

if [ "$NOTARY_KEYCHAIN_PROFILE" != "" ]; then
  echo Uploading to App Notary service
  xcrun notarytool submit --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" --wait $INSTALLER_FOLDER/Moonlight.dmg || fail "Notary submission failed"

  echo Stapling notary ticket to DMG
  xcrun stapler staple -v $INSTALLER_FOLDER/Moonlight.dmg || fail "Notary ticket stapling failed!"
fi

mv $INSTALLER_FOLDER/Moonlight.dmg $INSTALLER_FOLDER/Moonlight-$VERSION.dmg
echo Build successful
