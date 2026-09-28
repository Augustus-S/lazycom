#!/usr/bin/env bash
set -euo pipefail

output_dir=$(realpath -m "${1:?Usage: build-srpm.sh OUTPUT_DIR [SPEC_FILE]}")
repo_dir=$(git -C "$(dirname -- "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
spec_file=${2:-packaging/lazycom.spec}
cd "$repo_dir"
spec_file=$(realpath --relative-to="$repo_dir" "$spec_file")

if ! git diff --quiet HEAD --; then
  echo 'Commit tracked changes before generating an SRPM; only HEAD is archived.' >&2
  exit 1
fi
commit=$(git rev-parse --verify 'HEAD^{commit}')
git cat-file -e "$commit:$spec_file"
version=$(git show "$commit:CMakeLists.txt" | awk '$1 == "project(lazycom" && $2 == "VERSION" {print $3}')
spec_version=$(git show "$commit:$spec_file" | awk '$1 == "%global" && $2 == "upstream_version" {print $3}')
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ || $version != "$spec_version" ]]; then
  echo 'CMake project version and spec upstream_version must match (X.Y.Z).' >&2
  exit 1
fi

suffix=
if [[ $(git rev-parse --verify "refs/tags/v$version^{commit}" 2>/dev/null || true) != "$commit" ]]; then
  timestamp=$(git show -s --format=%ct "$commit")
  timestamp=$(date -u -d "@$timestamp" +%Y%m%d%H%M%S)
  suffix="~pre.$timestamp.g${commit:0:12}"
fi
package_version="$version$suffix"
spec_suffix='%{nil}'
if [[ -n $suffix ]]; then
  spec_suffix=$suffix
fi

mkdir -p "$output_dir"
staging=$(mktemp -d "$output_dir/.srpm.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT
mkdir -p "$staging/SOURCES" "$staging/SPECS"
git archive --format=tar --prefix="lazycom-$package_version/" "$commit" |
  gzip -n > "$staging/SOURCES/lazycom-$package_version.tar.gz"
git show "$commit:$spec_file" |
  sed -e "s/^%global source_commit .*/%global source_commit $commit/" \
      -e "s/^%global package_suffix .*/%global package_suffix $spec_suffix/" \
  > "$staging/SPECS/lazycom.spec"

rpmbuild -bs --define "_topdir $staging" --define "_srcrpmdir $output_dir" \
  "$staging/SPECS/lazycom.spec"
