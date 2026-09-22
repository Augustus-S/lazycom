#!/usr/bin/env bash
set -euo pipefail

build_dir=$(realpath "${1:?Usage: build-appimage.sh BUILD_DIR OUTPUT_DIR}")
output_dir=$(realpath -m "${2:?Usage: build-appimage.sh BUILD_DIR OUTPUT_DIR}")
tools_dir=$(realpath -m "${APPIMAGE_TOOL_DIR:-$build_dir/appimage-tools}")
version=$(cat "$build_dir/package-version.txt")
[[ $(uname -m) == x86_64 ]] || { echo 'AppImage packaging requires x86_64' >&2; exit 1; }
mkdir -p "$output_dir" "$tools_dir"

fetch_tool() {
  local url=$1 digest=$2 target="$tools_dir/$3"
  if [[ ! -f "$target" ]]; then
    curl --fail --location --retry 3 "$url" --output "$target.tmp"
    printf '%s  %s\n' "$digest" "$target.tmp" | sha256sum --check
    mv "$target.tmp" "$target"
  fi
  printf '%s  %s\n' "$digest" "$target" | sha256sum --check
}

fetch_tool \
  https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
  ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 \
  appimagetool-x86_64.AppImage
fetch_tool \
  https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 \
  2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d \
  runtime-x86_64
chmod +x "$tools_dir/appimagetool-x86_64.AppImage"

staging=$(mktemp -d "$build_dir/appimage-staging.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT
appdir="$staging/lazycom.AppDir"
cmake --install "$build_dir" --component Runtime --prefix "$appdir/usr" --strip
ln -s usr/bin/lazycom "$appdir/AppRun"
ln -s usr/share/applications/lazycom.desktop "$appdir/lazycom.desktop"
ln -s usr/share/icons/hicolor/scalable/apps/lazycom.svg "$appdir/lazycom.svg"
ln -s lazycom.svg "$appdir/.DirIcon"
desktop-file-validate "$appdir/lazycom.desktop"

ARCH=x86_64 VERSION="$version" "$tools_dir/appimagetool-x86_64.AppImage" \
  --appimage-extract-and-run --no-appstream \
  --runtime-file "$tools_dir/runtime-x86_64" \
  --mksquashfs-opt -processors --mksquashfs-opt 2 \
  "$appdir" "$output_dir/lazycom-$version-x86_64.AppImage"
