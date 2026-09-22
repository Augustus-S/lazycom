#!/usr/bin/env bash
set -euo pipefail

format=${1:?Usage: test-install.sh deb|rpm ASSET_DIR}
assets=$(realpath "${2:?Usage: test-install.sh deb|rpm ASSET_DIR}")
scripts=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
shopt -s nullglob
packages=("$assets"/*."$format")
[[ ${#packages[@]} == 1 ]] || { echo 'Expected exactly one native package' >&2; exit 1; }

case "$format" in
  deb)
    export DEBIAN_FRONTEND=noninteractive
    apt-get -o Acquire::Retries=3 update
    apt-get -o Acquire::Retries=3 install --no-install-recommends -y python3 "${packages[0]}"
    [[ $(dpkg-query -W -f='${Package}' lazycom) == lazycom ]]
    ;;
  rpm)
    dnf install -y python3 "${packages[0]}"
    [[ $(rpm -q --qf '%{NAME}' lazycom) == lazycom ]]
    ;;
  *) echo "Unsupported package format: $format" >&2; exit 1 ;;
esac

[[ $(command -v lazycom) == /usr/bin/lazycom ]]
ldd /usr/bin/lazycom | tee /tmp/lazycom-ldd.txt
grep -F '/usr/bin/../lib/lazycom/libserialport.so.0' /tmp/lazycom-ldd.txt
if grep -F 'not found' /tmp/lazycom-ldd.txt; then exit 1; fi
test -f /usr/share/lazycom/source/libserialport/configure
python3 "$scripts/smoke-test.py" /usr/bin/lazycom

case "$format" in
  deb) apt-get remove -y lazycom ;;
  rpm) dnf remove -y lazycom ;;
esac
test ! -e /usr/bin/lazycom
test ! -e /usr/lib/lazycom/libserialport.so.0

images=("$assets"/*.AppImage)
[[ ${#images[@]} == 1 ]] || { echo 'Expected exactly one AppImage' >&2; exit 1; }
cp "${images[0]}" /tmp/lazycom.AppImage
chmod 755 /tmp/lazycom.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 python3 "$scripts/smoke-test.py" /tmp/lazycom.AppImage
