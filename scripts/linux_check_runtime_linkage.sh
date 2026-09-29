#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
#
# Fails when the Linux runtime links an FFmpeg library, directly (NEEDED) or through
# a dependency (ldd). --self-test also plants a NEEDED libavcodec entry into a copy
# of the library with patchelf and requires this check to reject it.
#
#   scripts/linux_check_runtime_linkage.sh <liboxrsys-runtime.so> [--self-test]

set -euo pipefail

BANNED='lib(avcodec|avformat|avutil|avfilter|avdevice|swscale|swresample|postproc)'

check() {
    local library="$1"
    if [[ ! -f "$library" ]]; then
        echo "FAIL: $library does not exist"
        return 1
    fi
    local needed linked
    needed=$(readelf -d "$library" | grep NEEDED || true)
    linked=$(ldd "$library" 2>&1 || true)
    if [[ -z "$needed" ]]; then
        echo "FAIL: readelf lists no NEEDED entries for $library"
        return 1
    fi
    if ! grep -q 'libc\.so' <<<"$needed"; then
        echo "FAIL: NEEDED has no libc entry, so this is not reading $library's dynamic section"
        return 1
    fi
    local hits
    hits=$( (grep -oE "$BANNED[.a-z0-9_-]*" <<<"$needed"; grep -oE "$BANNED[.a-z0-9_-]*" <<<"$linked") | sort -u || true)
    if [[ -n "$hits" ]]; then
        echo "FAIL: $library links FFmpeg: $(tr '\n' ' ' <<<"$hits")"
        return 1
    fi
    echo "PASS: $library links no FFmpeg library ($(grep -c NEEDED <<<"$needed") NEEDED entries)"
}

library="${1:?usage: $0 <liboxrsys-runtime.so> [--self-test]}"
check "$library"

if [[ "${2:-}" == "--self-test" ]]; then
    scratch=$(mktemp -d)
    trap 'rm -rf "$scratch"' EXIT
    cp "$library" "$scratch/planted.so"
    patchelf --add-needed libavcodec.so.61 "$scratch/planted.so"
    if check "$scratch/planted.so"; then
        echo "FAIL: control: a planted NEEDED libavcodec entry was not caught"
        exit 1
    fi
    echo "PASS: control: the planted libavcodec entry was caught"
    if check "$scratch/missing.so"; then
        echo "FAIL: control: a missing library passed"
        exit 1
    fi
    echo "PASS: control: a missing library fails"
fi
