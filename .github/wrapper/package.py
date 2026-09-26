#!/usr/bin/env python3
"""Package the custom wrapper: base package files plus our libvulkan_wrapper.so.

The auxiliary hook libraries and libadrenotools.so come from the base package
unchanged.  Only libvulkan_wrapper.so is replaced, and the ELF is checked
against the libraries the container actually provides before the archive is
written.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
work = ROOT / 'ci-wrapper'
base = work / 'base-package'
out = ROOT / 'out-wrapper'
if out.exists():
    shutil.rmtree(out)
out.mkdir()
staging = out / 'package'
shutil.copytree(base, staging)

ndkbin = Path(os.environ['PANVK_NDK']) / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
readelf = str(ndkbin / 'llvm-readelf')
strip = str(ndkbin / 'llvm-strip')
api = int(os.environ.get('ANDROID_API', '26'))

so = staging / 'usr/lib/libvulkan_wrapper.so'
built = ROOT / 'build-wrapper-android/src/vulkan/wrapper/libvulkan_wrapper.so'
if not built.is_file():
    raise SystemExit(f'missing {built}')
shutil.copy2(built, so)
subprocess.run([strip, '--strip-unneeded', str(so)], check=True)


def inspect(*args):
    return subprocess.check_output([readelf, *args, str(so)], text=True)


header, notes, dynamic, symbols = inspect('-h'), inspect('-n'), inspect('-d'), inspect('--dyn-syms', '--wide')
(out / 'elf-report.txt').write_text(header + '\n' + notes + '\n' + dynamic + '\n' + symbols)
if 'AArch64' not in header or 'ELF64' not in header:
    raise SystemExit('Not an ELF64 AArch64 library')
if '(RPATH)' in dynamic or '(RUNPATH)' in dynamic:
    raise SystemExit('Unexpected runtime library search path remains')
if re.search(r'@GLIBC_|@GLIBCXX_', symbols):
    raise SystemExit('Linux/glibc symbol version found in Android output')
min_sdk = re.search(r'Android, API (\d+)', notes)
if not min_sdk or int(min_sdk.group(1)) > api:
    raise SystemExit(f'unexpected Android API level: {min_sdk and min_sdk.group(1)} (target {api})')

# Android platform libraries plus the X11/DRM set deployed with the driver.
allowed = {
    'libc.so', 'libdl.so', 'libm.so', 'liblog.so', 'libz.so',
    'libcutils.so', 'libsync.so', 'libnativewindow.so', 'libhardware.so',
    'libandroid.so', 'libadrenotools.so',
    'libxcb.so', 'libxcb-dri3.so', 'libxcb-present.so', 'libxcb-sync.so',
    'libxcb-randr.so', 'libxcb-shm.so', 'libxcb-dri2.so', 'libxcb-keysyms.so',
    'libX11-xcb.so', 'libdrm.so', 'libxshmfence.so',
}
needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
unexpected = sorted(set(needed) - allowed)
if unexpected:
    raise SystemExit(f'Runtime libraries not present in the container: {unexpected}')
for required in ('vk_icdGetInstanceProcAddr', 'vk_icdNegotiateLoaderICDInterfaceVersion',
                 'vkGetInstanceProcAddr'):
    matching = [line for line in symbols.splitlines()
                if line.split() and line.split()[-1].split('@')[0] == required
                and ' UND ' not in line and ' GLOBAL ' in line and ' DEFAULT ' in line]
    if not matching:
        raise SystemExit(f'Missing public defined symbol: {required}')

# The ICD manifest of the base package is kept verbatim; log any drift.
icd = staging / 'usr/share/vulkan/icd.d/wrapper_icd.aarch64.json'
manifest = json.loads(icd.read_text())
if Path(manifest['ICD']['library_path']).name != so.name:
    raise SystemExit(f'unexpected ICD library_path: {manifest["ICD"]["library_path"]}')
generated = ROOT / 'build-wrapper-android/src/vulkan/wrapper/wrapper_icd.aarch64.json'
if generated.is_file():
    (out / 'generated-wrapper-icd.json').write_text(generated.read_text())
(out / 'runtime-dependencies.txt').write_text('\n'.join(sorted(set(needed))) + '\n')

sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
wrapper_sha = (work / 'provenance.txt').read_text().splitlines()[1]
readme = f'''Custom bionic Vulkan wrapper for WinatorMali containers on Mali-G57.
PanVK repository commit: {sha}
leegao bionic-vulkan-wrapper commit: {wrapper_sha}
Change: prefer native host import; drop VK_EXT_map_memory_placed when the driver
exposes VK_EXT_external_memory_host, so WRAPPER_EXTENSION_BLACKLIST is no longer
needed.  Every other base package file is byte-for-byte the original.

Import this archive through the app's wrapper/driver package selector, not as
the graphics driver.  Select the matching PanVK driver below it.
'''
(out / 'LEIA-ME.txt').write_text(readme)
shutil.copy2(work / 'provenance.txt', out / 'build-provenance.txt')

archive = out / f'wrapper-prefer-native-host-import-{sha[:8]}.tzst'
subprocess.run(['tar', '--zstd', '-cf', str(archive), '--owner=0', '--group=0',
                '--numeric-owner', '--mtime=@0', '--sort=name',
                '-C', str(staging), '.'], check=True)
with (out / 'SHA256SUMS').open('w') as f:
    for p in sorted(out.iterdir()):
        if p.is_file() and p.name != 'SHA256SUMS':
            f.write(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n')
print('Wrapper packaged:', archive)
print('Runtime dependencies:', ', '.join(sorted(set(needed))))
print('ICD:', manifest['ICD']['library_path'], manifest['ICD'].get('api_version', ''))
