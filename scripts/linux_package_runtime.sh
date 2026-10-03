#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Packages the Linux runtime as .deb and .rpm with nFPM (packaging/nfpm.yaml): the library and its
# OpenXR manifest under /opt/oxrsys. Installing does not make it the active runtime; OXRSys Home or
# XR_RUNTIME_JSON does.
#   scripts/linux_package_runtime.sh <liboxrsys-runtime.so> <out-dir>
#   scripts/linux_package_runtime.sh --check <package>   # fails unless the package holds both files
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
prefix=/opt/oxrsys
manifest_path="$prefix/share/openxr/1/oxrsys-runtime.json"
library_path="$prefix/lib/liboxrsys-runtime.so"

check() {
    local package="$1" listing
    case "$package" in
        *.deb) listing="$(dpkg-deb -c "$package" | awk '{print $NF}' | sed 's#^\./#/#')" ;;
        *.rpm) listing="$(rpm -qlp "$package")" ;;
        *) echo "FAIL: unknown package type $package"; return 1 ;;
    esac
    for wanted in "$library_path" "$manifest_path"; do
        if ! grep -qx "$wanted" <<<"$listing"; then
            echo "FAIL: $package lacks $wanted"
            return 1
        fi
    done
    echo "PASS: $package holds $library_path and $manifest_path"
}

if [[ "${1:-}" == "--check" ]]; then
    check "$2"
    exit $?
fi

library="$1"
out="$2"
version="$(sed -n 's/^OXRSYS_VERSION *= *//p' "$root/config/OXRSysVersion.xcconfig")"
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

install -Dm755 "$library" "$staging$library_path"
install -d "$(dirname "$staging$manifest_path")"
cat >"$staging$manifest_path" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "OXRSys Runtime",
        "library_path": "$library_path"
    }
}
EOF

mkdir -p "$out"
export OXRSYS_STAGE="$staging" OXRSYS_PKG_VERSION="$version"
for type in deb rpm; do
    nfpm package -f "$root/packaging/nfpm.yaml" -p "$type" -t "$out"
done
ls -1 "$out"
