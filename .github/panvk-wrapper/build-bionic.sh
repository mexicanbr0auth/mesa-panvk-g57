#!/usr/bin/env bash
set -euo pipefail
SRC="${1:?}" OUT="${2:?}" NDK="${ANDROID_NDK_LATEST_HOME:?}"
cd "$SRC"
CF="$PWD/android-cross.txt"
cat > "$CF" <<EOF
[binaries]
ar = '$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar'
c = ['$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang']
cpp = ['$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang++']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
[properties]
needs_exe_wrapper = true
EOF
meson setup _build --cross-file "$CF" -Dbuildtype=debugoptimized \
 -Dplatforms=android,x11 -Dplatform-sdk-version=35 -Dandroid-stub=true \
 -Dandroid-libbacktrace=disabled -Dglx=disabled -Dgbm=disabled -Degl=disabled \
 -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dglvnd=disabled \
 -Dllvm=disabled -Dvalgrind=disabled -Dgallium-drivers= -Dshared-glapi=disabled \
 -Dzstd=disabled -Dvulkan-drivers=wrapper
meson compile -C _build -j2
mkdir -p "$OUT"; SO=_build/src/vulkan/wrapper/libvulkan_wrapper.so
cp "$SO" "$OUT/libvulkan_wrapper.debug.so"; cp "$SO" "$OUT/libvulkan_wrapper.so"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" "$OUT/libvulkan_wrapper.so"
file "$OUT/libvulkan_wrapper.so"
