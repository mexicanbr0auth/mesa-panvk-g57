#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
ROOT="$PWD"
WORK="$ROOT/ci-winlatormali"
mkdir -p "$WORK"
exec > >(tee "$WORK/build.log") 2>&1
: "${PANVK_NDK:?Set PANVK_NDK to the NDK directory}"
export ANDROID_API="${ANDROID_API:-35}"
BUILD_JOBS="${BUILD_JOBS:-2}"
NDKBIN="$PANVK_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
test -x "$NDKBIN/aarch64-linux-android${ANDROID_API}-clang"

# The imported local JM files are preserved byte-for-byte.
sha256sum -c .github/winlatormali/local-sources.sha256
if rg -n 'diag_meta_vb_cpu|diagnostic_padding|VBCHK|VBPRESUB' \
    src/panfrost/vulkan/jm/panvk_cmd_buffer.h \
    src/panfrost/vulkan/jm/panvk_vX_cmd_draw.c \
    src/panfrost/vulkan/jm/panvk_vX_gpu_queue_kbase.c \
    src/vulkan/runtime/vk_meta_draw_rects.c; then
  echo 'Temporary texture diagnostic found; refusing to package.' >&2
  exit 1
fi

{
  git rev-parse HEAD
  cat "$PANVK_NDK/source.properties"
  meson --version
  "$NDKBIN/clang" --version
  /usr/bin/llvm-config-19 --version
  dpkg-query -W clang-19 llvm-19-dev libclc-19 libllvmspirvlib-19-dev
} > "$WORK/provenance.txt"

# Ubuntu's SPIRV-Tools may predate this Mesa tree's >=2024.1 requirement.
# Build real host libraries, never Android substitutes.
SDK_TAG=vulkan-sdk-1.4.309.0
HOST_DEPS="$WORK/host-deps"
git clone --depth 1 --branch "$SDK_TAG" \
  https://github.com/KhronosGroup/SPIRV-Tools.git "$WORK/SPIRV-Tools"
git clone --depth 1 --branch "$SDK_TAG" \
  https://github.com/KhronosGroup/SPIRV-Headers.git \
  "$WORK/SPIRV-Tools/external/spirv-headers"
test "$(git -C "$WORK/SPIRV-Tools" rev-parse "refs/tags/$SDK_TAG")" = f289d047f49fb60488301ec62bafab85573668cc
test "$(git -C "$WORK/SPIRV-Tools/external/spirv-headers" rev-parse "refs/tags/$SDK_TAG")" = 09913f088a1197aba4aefd300a876b2ebbaa3391
git -C "$WORK/SPIRV-Tools" rev-parse HEAD >> "$WORK/provenance.txt"
git -C "$WORK/SPIRV-Tools/external/spirv-headers" rev-parse HEAD >> "$WORK/provenance.txt"
cmake -S "$WORK/SPIRV-Tools" -B "$WORK/spirv-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOST_DEPS" \
  -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON
cmake --build "$WORK/spirv-build" --parallel "$BUILD_JOBS"
cmake --install "$WORK/spirv-build"

export PATH="/usr/lib/llvm-19/bin:$PATH"
export PKG_CONFIG_PATH="$HOST_DEPS/lib/pkgconfig:/usr/lib/llvm-19/lib/pkgconfig"
export LD_LIBRARY_PATH="$HOST_DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LLVM_CONFIG=/usr/bin/llvm-config-19

COMMON=(--buildtype=release -Dbuild-tests=false -Dgallium-drivers= \
  -Dvulkan-drivers=panfrost -Dpanfrost-kmds=kbase,panthor \
  -Dpanvk-use-kbase=true -Dpanfrost-rust=false \
  -Degl=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dopengl=false -Dgbm=disabled -Dperfetto=false -Dvideo-codecs= \
  -Dshader-cache=disabled -Dxmlconfig=disabled -Dexpat=disabled \
  -Dlibunwind=disabled -Dzstd=disabled)

# These binaries execute on the runner, not on the phone.
CC=clang-19 CXX=clang++-19 meson setup build-winlatormali-native \
  "${COMMON[@]}" -Dplatforms= -Dllvm=enabled \
  -Dmesa-clc=enabled -Dprecomp-compiler=enabled
meson compile -C build-winlatormali-native -j "$BUILD_JOBS" \
  mesa_clc vtn_bindgen2 panfrost_compile

mkdir -p "$WORK/host-bin"
python3 - "$ROOT/build-winlatormali-native" "$WORK/host-bin" <<'PY'
import pathlib, sys
root, dst = map(pathlib.Path, sys.argv[1:])
for name in ('mesa_clc', 'vtn_bindgen2', 'panfrost_compile'):
    found = [p for p in root.rglob(name) if p.is_file()]
    if len(found) != 1:
        raise SystemExit(f'Expected one host executable {name}: {found}')
    (dst / name).symlink_to(found[0])
PY
export PATH="$WORK/host-bin:$PATH"

# Isolate Android pkg-config from all Linux libraries.
mkdir -p "$WORK/empty-pkgconfig"
export PANVK_CROSS_FILE="$WORK/android-aarch64.ini"
export PANVK_CROSS_PKGDIR="$WORK/empty-pkgconfig"
python3 - <<'PY'
import os
from pathlib import Path
ndk = Path(os.environ['PANVK_NDK']) / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
api = os.environ.get('ANDROID_API', '35')
text = f'''[binaries]
c = '{ndk}/aarch64-linux-android{api}-clang'
cpp = '{ndk}/aarch64-linux-android{api}-clang++'
ar = '{ndk}/llvm-ar'
strip = '{ndk}/llvm-strip'
pkg-config = '/usr/bin/pkgconf'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['{os.environ['PANVK_CROSS_PKGDIR']}']

[built-in options]
c_args = ['-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
cpp_args = ['-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
cpp_link_args = ['-static-libstdc++']
pkg_config_path = []
'''
Path(os.environ['PANVK_CROSS_FILE']).write_text(text)
PY

# Mesa's android-stub libraries are link-time ABI placeholders ONLY.
# They are never included in the output package or used on the device.
# Actual Android system libraries must resolve these symbols at runtime.
PKG_CONFIG_PATH= meson setup build-winlatormali-android \
  --cross-file "$PANVK_CROSS_FILE" "${COMMON[@]}" \
  -Dplatforms=android -Dplatform-sdk-version="$ANDROID_API" \
  -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
  -Dandroid-libperfetto=disabled -Dzlib=disabled \
  -Dllvm=disabled -Dspirv-tools=disabled -Dcpp_rtti=false \
  -Dmesa-clc=system -Dprecomp-compiler=system \
  --force-fallback-for=libdrm -Dallow-fallback-for=libdrm \
  -Dlibdrm:default_library=static
meson compile -C build-winlatormali-android -j "$BUILD_JOBS" vulkan_panfrost
