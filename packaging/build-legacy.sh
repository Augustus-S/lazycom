#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_SHA:?Source commit required}"
: "${GITHUB_REF_TYPE:?Ref type required}"
: "${GITHUB_REF_NAME:?Ref name required}"
version=$(awk '$1 == "project(lazycom" && $2 == "VERSION" {print $3}' CMakeLists.txt)
suffix="~pre.${GITHUB_RUN_NUMBER}.${GITHUB_RUN_ATTEMPT}.g${GITHUB_SHA:0:12}"
if [[ "$GITHUB_REF_TYPE" == tag ]]; then
  if [[ "$GITHUB_REF_NAME" != "v$version" &&
        ! ( "$version" == *.0 && "$GITHUB_REF_NAME" == "v${version%.0}" ) ]]; then
    echo "Release tag must match project version $version" >&2
    exit 1
  fi
  suffix=
fi
cmake --preset gcc-release \
  -DCMAKE_C_COMPILER=/opt/gcc/bin/gcc -DCMAKE_CXX_COMPILER=/opt/gcc/bin/g++ \
  -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc' \
  -DLAZYCOM_GCC_RUNTIME_LICENSE_DIR=/opt/gcc/licenses \
  -DLAZYCOM_BUILD_COMMIT="$GITHUB_SHA" -DLAZYCOM_PACKAGE_SUFFIX="$suffix"
cmake --build --preset gcc-release
for generator in DEB RPM TGZ; do
  cpack --config build/gcc-release/CPackConfig.cmake -G "$generator" -B build/packages
done
# AppImage staging runs on the host; keep the runtime license inputs in the workspace.
mkdir -p build/gcc-runtime-licenses
cp /opt/gcc/licenses/* build/gcc-runtime-licenses/
cmake --preset gcc-release \
  -DLAZYCOM_GCC_RUNTIME_LICENSE_DIR="$PWD/build/gcc-runtime-licenses"
mkdir -p build/release-assets
cp build/packages/*.deb build/packages/*.rpm build/packages/*.tar.gz build/release-assets/
