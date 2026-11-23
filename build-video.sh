#!/bin/sh
set -eu
cd "$(dirname "$0")"
: "${GAZELLE_WORK:?Set GAZELLE_WORK to the prepared Gazelle kernel work directory}"
for profile in ps7702 ps7711 ps7712 ps7713 ps7714_5503 ps7714_5506 ps7714_5507 ps7715 ps7717 ps7717_5746 ps7718; do
    case "$profile" in ps7702) build=gazelle-fel-7702;; ps7711) build=gazelle-fel-7711;; ps7712) build=gazelle-fel-7712;; ps7713) build=gazelle-fel-7713;; ps7714_5503) build=gazelle-fel-7714-5503;; ps7714_5506) build=gazelle-fel-7714-5506;; ps7714_5507) build=gazelle-fel-7714-5507;; ps7715) build=gazelle-fel-7715;; ps7717) build=gazelle-fel-7717;; ps7717_5746) build=gazelle-fel-7717-5746;; ps7718) build=gazelle-fel-7718;; esac
    docker run --rm -v "$GAZELLE_WORK:/workspace" -v "$PWD:/module-src" gazelle-fel-builder:local \
      make -C /workspace/gazelle-fel-7702/source/kernel/amlogic/pop1g/5.4/common \
      O="/workspace/$build/kbuild" ARCH=arm64 CC=clang LD=ld.lld HOSTCC=clang LLVM=1 \
      CONFIG_AMLOGIC_LTO_CLANG= CONFIG_LTO= M="/module-src/src/fel/$profile" modules
    mkdir -p "video/fel/$profile"
    cp "src/fel/$profile/dvfel.ko" "video/fel/$profile/"
done
python3 video-build-inputs.py record
