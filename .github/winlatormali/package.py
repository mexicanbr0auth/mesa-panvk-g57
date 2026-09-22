#!/usr/bin/env python3
"""Package only the real PanVK library, never Mesa's link-time stub libraries."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
ndkbin = Path(os.environ['PANVK_NDK']) / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
readelf = str(ndkbin / 'llvm-readelf')
out = ROOT / 'out-winlatormali'
out.mkdir(exist_ok=True)
so = out / 'libvulkan_panfrost.so'
shutil.copy2(ROOT / 'build-winlatormali-android/src/panfrost/vulkan/libvulkan_panfrost.so', so)

# Meson may embed paths to link-time Android stub libraries. Never ship them.
subprocess.run(['patchelf', '--remove-rpath', str(so)], check=True)
subprocess.run([str(ndkbin / 'llvm-strip'), '--strip-unneeded', str(so)], check=True)
def inspect(*args):
    return subprocess.check_output([readelf, *args, str(so)], text=True)
header, dynamic, symbols = inspect('-h'), inspect('-d'), inspect('--dyn-syms', '--wide')
(out / 'elf-report.txt').write_text(header + '\n' + dynamic + '\n' + symbols)
if 'AArch64' not in header or 'ELF64' not in header:
    raise SystemExit('Not an ELF64 AArch64 library')
if '(RPATH)' in dynamic or '(RUNPATH)' in dynamic:
    raise SystemExit('Unexpected runtime library search path remains')
needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
allowed = {'libc.so', 'libm.so', 'libdl.so', 'liblog.so', 'libandroid.so',
           'libnativewindow.so', 'libhardware.so', 'libsync.so', 'libcutils.so', 'libz.so'}
if set(needed) - allowed:
    raise SystemExit(f'Unexpected runtime dependencies: {sorted(set(needed) - allowed)}')
for required in ('HMI', 'vk_icdGetInstanceProcAddr'):
    matching = [line for line in symbols.splitlines()
                if line.split() and line.split()[-1].split('@')[0] == required
                and ' UND ' not in line and ' GLOBAL ' in line and ' DEFAULT ' in line]
    if not matching:
        raise SystemExit(f'Missing public defined symbol: {required}')
if re.search(r'@GLIBC_|@GLIBCXX_', symbols):
    raise SystemExit('Linux/glibc symbol version found in Android output')

sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
meta = {
    'schemaVersion': 1,
    'name': f'PanVK G57 WinlatorMali TEST {sha[:8]}',
    'description': 'Experimental Android kbase/JM build. AHB and Winlator presentation need device validation. Known GFX copy corruption remains.',
    'author': 'Mesa contributors / mexicanbr0auth',
    'packageVersion': 100,
    'vendor': 'Mesa',
    'driverVersion': f'{(ROOT / "VERSION").read_text().strip()}-{sha[:8]}',
    'minApi': int(os.environ.get('ANDROID_API', '35')),
    'libraryName': so.name,
}
(out / 'meta.json').write_text(json.dumps(meta, indent=2) + '\n')
notice = '''EXPERIMENTAL: this is not a texture fix or a validated Winlator release.
Import the inner driver ZIP through the AdrenoTools driver manager, not as a wrapper.
Preserve Winlator's wrapper; select this custom driver beneath it.
The ZIP includes only meta.json and libvulkan_panfrost.so.
Android link-time stub libraries MUST NOT be copied to the device.
System libhardware/libnativewindow/libsync availability in the loader namespace still needs testing.
AHB/gralloc import and swapchain presentation need validation on Mali-G57.
Do not enable PANVK_JM_PIPELINE for baseline testing; the optional experiment remains in source.
Do not force a Vulkan version or change advertised features to bypass initialization errors.
'''
(out / 'LEIA-ME.txt').write_text(notice)
shutil.copy2(ROOT / 'ci-winlatormali/provenance.txt', out / 'build-provenance.txt')
shutil.copy2(ROOT / '.github/winlatormali/local-sources.sha256', out / 'local-sources.sha256')
archive = out / f'PanVK-G57-WinlatorMali-TEST-{sha[:8]}.zip'
with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
    z.write(so, so.name)
    z.write(out / 'meta.json', 'meta.json')
with (out / 'SHA256SUMS').open('w') as f:
    for p in sorted(out.iterdir()):
        if p.is_file() and p.name != 'SHA256SUMS':
            f.write(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n')
print('Candidate packaged:', archive)
print('Runtime dependencies:', ', '.join(needed))
