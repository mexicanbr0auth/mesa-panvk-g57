# Custom Vulkan wrapper build (GitHub Actions)

Builds `libvulkan_wrapper.so` from the audited leegao bionic wrapper, with the
`wrapper-prefer-native-host-import` patch applied, and packages it as a
`wrapper.tzst` for the WinatorMali container. Nothing is built on the phone.

## Inputs

All inputs are pinned; override only when auditing a new upstream commit.

| Input | Default |
| --- | --- |
| `WRAPPER_REPO` / `WRAPPER_BRANCH` | `https://github.com/leegao/bionic-vulkan-wrapper.git`, `wrapper` |
| `WRAPPER_COMMIT` | `c8baafbd4f4835ca103acb55ec3ac13642b6b7e3` (2025-08-22) |
| `WRAPPER_BASE_URL` | `app/src/main/assets/graphics_driver/wrapper.tzst` from `GunaCharanTeja/WinlatorMali` branch `bionic-mali-1.0` |
| `ANDROID_API` | `26` (Meson requires at least 25) |
| NDK | r28c, same as the PanVK driver workflow |

The base package supplies `libadrenotools.so` (a link-time dependency of the
wrapper) and the auxiliary hook libraries. Only `libvulkan_wrapper.so` is
replaced; every other file is shipped byte-for-byte, including the ICD manifest
and `version.txt`.

The app's `libadrenotools.so` exports every entry point leegao's source calls
(`adrenotools_open_libvulkan`, `adrenotools_import_user_mem`,
`adrenotools_mem_gpu_allocate`, `adrenotools_mem_cpu_map`,
`adrenotools_validate_gpu_mapping`, `adrenotools_set_turbo`), so the app
package is a valid base even for a leegao wrapper build. Point
`WRAPPER_BASE_URL` at another `wrapper.tzst` only if the container ships
different hook libraries.

X11/XCB, libdrm and xshmfence headers plus link objects come from the Termux
`aarch64` packages. They already exist inside the container, so nothing fetched
here is packaged: `fetch-termux-debs.py` only rewrites the `.pc` prefixes, drops
unresolvable private requirements, removes `xcb-keysyms.pc` and trims
`x11-xcb.pc` so plain `libX11` never reaches `DT_NEEDED`.

## The patch

`wrapper-prefer-native-host-import.patch` clears
`VK_EXT_map_memory_placed` only when the driver exposes
`VK_EXT_external_memory_host` and does not expose `VK_EXT_map_memory_placed`.
Wine then uses native host import for WoW64 mappings below 4 GiB.

`package.py` refuses to build an archive if the resulting library needs any
runtime library outside the set the container provides.

## Using the result

1. Install the wrapper `.tzst` through the app's wrapper package selector.
2. Select the PanVK driver built by `build-winlatormali.yml` beneath it.
3. Only after the wrapper works, drop
   `WRAPPER_EXTENSION_BLACKLIST=VK_EXT_map_memory_placed` and
   `PANVK_TEST_HOST_IMPORT=1` from the container environment; the driver
   enables host import by default since `9ac3cce`.
