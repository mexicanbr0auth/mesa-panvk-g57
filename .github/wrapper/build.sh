#!/usr/bin/env bash
# Build the bionic Vulkan wrapper (libvulkan_wrapper.so) for Android aarch64.
# Everything happens on the CI runner: the wrapper source, the static
# SPIRV-Tools libraries, the X11/DRM link dependencies and the final .tzst.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
ROOT="$PWD"
WORK="$ROOT/ci-wrapper"
rm -rf "$WORK"
mkdir -p "$WORK"
export WRAPPER_WORK="$WORK"
exec > >(tee "$WORK/build.log") 2>&1

: "${PANVK_NDK:?Set PANVK_NDK to the Android NDK directory}"
export ANDROID_API="${ANDROID_API:-26}"
BUILD_JOBS="${BUILD_JOBS:-4}"
NDKBIN="$PANVK_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
test -x "$NDKBIN/aarch64-linux-android${ANDROID_API}-clang"

# Pinned inputs.  The wrapper commit is the audited leegao branch tip.
WRAPPER_REPO="${WRAPPER_REPO:-https://github.com/leegao/bionic-vulkan-wrapper.git}"
WRAPPER_BRANCH="${WRAPPER_BRANCH:-wrapper}"
WRAPPER_COMMIT="${WRAPPER_COMMIT:-c8baafbd4f4835ca103acb55ec3ac13642b6b7e3}"
WRAPPER_BASE_URL="${WRAPPER_BASE_URL:-https://raw.githubusercontent.com/GunaCharanTeja/WinlatorMali/bionic-mali-1.0/app/src/main/assets/graphics_driver/wrapper.tzst}"
SDK_TAG=vulkan-sdk-1.4.309.0
SDK_TAG_SHA=f289d047f49fb60488301ec62bafab85573668cc
SDK_HEADERS_SHA=09913f088a1197aba4aefd300a876b2ebbaa3391

# 1. Base package: auxiliary hook libraries and libadrenotools.so, which is a
#    link-time dependency of the wrapper.  Only libvulkan_wrapper.so is
#    replaced later; every other file is shipped byte-for-byte.
mkdir -p "$WORK/base-package"
curl -L --fail --retry 3 -sS "$WRAPPER_BASE_URL" -o "$WORK/base-wrapper.tzst"
sha256sum "$WORK/base-wrapper.tzst" | tee "$WORK/base-wrapper.sha256"
tar --zstd -xf "$WORK/base-wrapper.tzst" -C "$WORK/base-package"
for required in usr/lib/libadrenotools.so usr/lib/libhook_impl.so \
    usr/lib/libmain_hook.so usr/lib/libgsl_alloc_hook.so \
    usr/lib/libfile_redirect_hook.so version.txt \
    usr/share/vulkan/icd.d/wrapper_icd.aarch64.json; do
  test -e "$WORK/base-package/$required" || {
    echo "base package is missing $required" >&2
    exit 1
  }
done

# 2. Audited wrapper source, patched to prefer native host import.
SRC="$WORK/wrapper-src"
git clone --depth 1 --branch "$WRAPPER_BRANCH" "$WRAPPER_REPO" "$SRC"
test "$(git -C "$SRC" rev-parse HEAD)" = "$WRAPPER_COMMIT"
git -C "$SRC" apply --check "$ROOT/.github/wrapper/wrapper-prefer-native-host-import.patch"
git -C "$SRC" apply "$ROOT/.github/wrapper/wrapper-prefer-native-host-import.patch"
git -C "$SRC" diff --stat

# 3. X11/XCB, libdrm and xshmfence headers plus link objects for Android.
#    These libraries already exist inside the container; nothing fetched here
#    is packaged, it only satisfies the compiler and the linker.
python3 .github/wrapper/fetch-termux-debs.py
export PKG_CONFIG_LIBDIR="$WORK/sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_PATH=

# 4. SPIRV-Tools static libraries, built with the same NDK as the wrapper.
#    The wrapper expects them in src/vulkan/wrapper/lib with the matching
#    headers, exactly like src/vulkan/wrapper/pull_spirv_tools.sh does.
git clone --depth 1 --branch "$SDK_TAG" \
  https://github.com/KhronosGroup/SPIRV-Tools.git "$WORK/SPIRV-Tools"
git clone --depth 1 --branch "$SDK_TAG" \
  https://github.com/KhronosGroup/SPIRV-Headers.git \
  "$WORK/SPIRV-Tools/external/spirv-headers"
test "$(git -C "$WORK/SPIRV-Tools" rev-parse HEAD)" = "$SDK_TAG_SHA"
test "$(git -C "$WORK/SPIRV-Tools/external/spirv-headers" rev-parse HEAD)" = "$SDK_HEADERS_SHA"
cmake -S "$WORK/SPIRV-Tools" -B "$WORK/spirv-android" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PANVK_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="android-${ANDROID_API}" \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON
cmake --build "$WORK/spirv-android" --parallel "$BUILD_JOBS" --target SPIRV-Tools-opt
mkdir -p "$SRC/src/vulkan/wrapper/lib"
rm -f "$SRC/src/vulkan/wrapper/lib"/*.a
for lib in SPIRV-Tools SPIRV-Tools-opt; do
  found="$(find "$WORK/spirv-android" -name "lib${lib}.a" -print -quit)"
  test -n "$found" || { echo "missing lib${lib}.a" >&2; exit 1; }
  cp "$found" "$SRC/src/vulkan/wrapper/lib/"
done
rm -rf "$SRC/src/vulkan/wrapper/include/spirv-tools"
cp -r "$WORK/SPIRV-Tools/include" "$SRC/src/vulkan/wrapper/include/spirv-tools"

# 5. Cross file.  __TERMUX__ is what the wrapper tree keys its container
#    specific WSI code (AHardwareBuffer images, X11 helpers) on, so the
#    cross build has to define it exactly like the phone build did.
#    fcntl.h is force included because wrapper_device_memory.c uses O_RDWR
#    and O_CLOEXEC without including it; on the phone toolchain the header
#    used to arrive transitively.  libadrenotools.so comes from the base package; Mesa's stub
#    Android libraries are link-time placeholders only, resolved by the real
#    platform libraries on the device.  -Wl,--as-needed keeps libraries we do
#    not call (libX11, xcb-keysyms) out of DT_NEEDED.
export WRAPPER_CROSS_FILE="$WORK/android-aarch64.ini"
python3 - <<'PY'
import os
from pathlib import Path

ndkbin = Path(os.environ['PANVK_NDK']) / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
api = os.environ.get('ANDROID_API', '26')
work = Path(os.environ['WRAPPER_WORK'])
baselib = work / 'base-package/usr/lib'
sysroot = work / 'sysroot/usr'
link_args = [
    '-static-libstdc++',
    '-Wl,--as-needed',
    f'-L{baselib}',
    f'-L{sysroot}/lib',
    '-ladrenotools',
]
quoted = ', '.join(f"'{a}'" for a in link_args)
Path(os.environ['WRAPPER_CROSS_FILE']).write_text(f"""[binaries]
c = '{ndkbin}/aarch64-linux-android{api}-clang'
cpp = '{ndkbin}/aarch64-linux-android{api}-clang++'
ar = '{ndkbin}/llvm-ar'
strip = '{ndkbin}/llvm-strip'
pkg-config = '/usr/bin/pkgconf'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-D__TERMUX__', '-include', 'fcntl.h', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
cpp_args = ['-D__TERMUX__', '-include', 'fcntl.h', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
c_link_args = [{quoted}]
cpp_link_args = [{quoted}]
pkg_config_path = []
""")
PY

# 6. Configure and build only the wrapper.  x11 keeps the XCB presentation path
#    the Winlator container relies on; android adds the AHardwareBuffer path.
#    xlib-lease only provides VK_EXT_acquire_xlib_display, which the
#    container never uses, and it would drag libXrandr into the build.
meson setup build-wrapper-android "$SRC" --cross-file "$WRAPPER_CROSS_FILE" \
  --prefix=/usr --libdir=lib --buildtype=release \
  -Dplatforms=android,x11 -Ddri3=enabled \
  -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
  -Dplatform-sdk-version="$ANDROID_API" \
  -Dvulkan-drivers=wrapper -Dvulkan-beta=false -Dbuild-tests=false \
  -Dgallium-drivers= -Dvideo-codecs= \
  -Degl=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled \
  -Dopengl=false -Dgbm=disabled \
  -Dllvm=disabled -Dvalgrind=disabled -Dperfetto=false \
  -Dshared-glapi=disabled -Dexpat=disabled -Dxmlconfig=disabled \
  -Dzstd=disabled -Dlibunwind=disabled -Dxlib-lease=disabled
meson compile -C build-wrapper-android -j "$BUILD_JOBS" vulkan_wrapper wrapper_icd

# 7. The patch must still be the only source change in the wrapper tree.
git -C "$SRC" status --short

{
  git rev-parse HEAD
  git -C "$SRC" rev-parse HEAD
  sha256sum .github/wrapper/wrapper-prefer-native-host-import.patch
  cat "$WORK/base-wrapper.sha256"
  cat "$PANVK_NDK/source.properties"
  meson --version
  "$NDKBIN/clang" --version
  cmake --version | head -1
  readelf --version | head -1
  dpkg-query -W glslang-tools pkgconf
} > "$WORK/provenance.txt"
cat "$WORK/provenance.txt"
