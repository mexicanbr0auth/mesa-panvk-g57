#!/usr/bin/env bash
set -euo pipefail
SRC="${1:?wrapper source dir}"
OUT="${2:?output dir}"
NDK="${ANDROID_NDK_LATEST_HOME:?}"
cd "$SRC"
export ANDROID_NDK_HOME="$NDK"
export MESON_WORKING_DIR="$PWD"
envsubst < android.toml > android-cross.toml
CLANG_VER="$(basename "$(ls -d "$NDK"/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/* | sort -V | tail -1)")"
sed -i "s|lib/clang/18|lib/clang/$CLANG_VER|g" android-cross.toml

meson setup _build \
  --cross-file android-cross.toml \
  -Dbuildtype=debugoptimized \
  -Dplatforms=android,x11 \
  -Dandroid-stub=true \
  -Dandroid-libbacktrace=disabled \
  -Dplatform-sdk-version=35 \
  -Dglx=disabled -Dgbm=disabled -Degl=disabled \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled \
  -Dgallium-drivers= -Dshared-glapi=disabled -Dzstd=disabled \
  -Dvulkan-drivers=wrapper
meson compile -C _build -j 2

SO=_build/src/vulkan/wrapper/libvulkan_wrapper.so
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
mkdir -p "$OUT"
cp "$SO" "$OUT/libvulkan_wrapper.debug.so"
cp "$SO" "$OUT/libvulkan_wrapper.so"
"$STRIP" "$OUT/libvulkan_wrapper.so"
file "$OUT/libvulkan_wrapper.so"
readelf -h "$OUT/libvulkan_wrapper.so" | grep -E 'Class:|Machine:'
