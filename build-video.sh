#!/bin/sh
set -eu
cd "$(dirname "$0")"
: "${GAZELLE_WORK:?Set GAZELLE_WORK to the prepared Gazelle kernel work directory}"
for profile in ps7702 ps7717; do
    case "$profile" in ps7702) build=gazelle-fel-7702;; ps7717) build=gazelle-fel-7717;; esac
    docker run --rm -v "$GAZELLE_WORK:/workspace" -v "$PWD:/module-src" gazelle-fel-builder:local \
      make -C /workspace/gazelle-fel-7702/source/kernel/amlogic/pop1g/5.4/common \
      O="/workspace/$build/kbuild" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang LLVM=1 \
      CONFIG_AMLOGIC_LTO_CLANG= CONFIG_LTO= M="/module-src/src/fel/$profile" modules
    mkdir -p "video/fel/$profile"
    cp "src/fel/$profile/dvfel.ko" "video/fel/$profile/"
done
python3 video-build-inputs.py record
