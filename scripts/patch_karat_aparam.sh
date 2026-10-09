#!/system/bin/sh
# Produce the tested overlay from this device's utility. Never modify the input.
# Magisk provides BusyBox ash standalone mode and its cp/dd/mktemp/sha256sum.
set -eu
[ "$#" -eq 2 ] || { echo "Usage: patch_karat_aparam.sh INPUT OUTPUT" >&2; exit 1; }
src=$1
dst=$2
stock=1ec08b6cdf633a683ace6123991b2b5b440f155804cd24b8081de287ad7e41b7
corrected=690cfd8c33e3c02d68c7e0d1c51415530907bcf41f1cf8784186ba17c897e4f2
actual=$(sha256sum "$src")
actual=${actual%% *}
case "$actual" in
    "$stock"|"$corrected") ;;
    *) echo "Unsupported Karat utility; no output installed." >&2; exit 1 ;;
esac
tmp=$(mktemp "${dst}.tmp.XXXXXX")
trap 'rm -f "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
cp "$src" "$tmp"
if [ "$actual" = "$stock" ]; then
    # Exact verified ELF32 layout: remove one 8-byte DT_NEEDED entry at 0x6e8.
    # Shift the remainder of PT_DYNAMIC (0x6f0..0x7ff) and add a null entry.
    # The stock hash above is mandatory before using these fixed offsets.
    dd if="$src" of="$tmp" bs=1 skip=1776 seek=1768 count=272 conv=notrunc 2>/dev/null
    dd if=/dev/zero of="$tmp" bs=1 seek=2040 count=8 conv=notrunc 2>/dev/null
fi
actual=$(sha256sum "$tmp")
actual=${actual%% *}
[ "$actual" = "$corrected" ] || { echo "Karat patch verification failed." >&2; exit 1; }
mv -f "$tmp" "$dst"
