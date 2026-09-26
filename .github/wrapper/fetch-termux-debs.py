#!/usr/bin/env python3
"""Fetch the Android aarch64 X11/DRM development files the wrapper links against.

The container already ships these libraries; they are needed here only as
headers, .pc files and link-time shared objects.  Nothing from this directory
is packaged into the wrapper .tzst.
"""
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

WORK = Path(os.environ['WRAPPER_WORK'])
SYSROOT = WORK / 'sysroot'
REPOS = {
    'termux-x11': 'https://packages.termux.dev/apt/termux-x11/dists/x11/main/binary-aarch64/Packages',
    'termux-main': 'https://packages.termux.dev/apt/termux-main/dists/stable/main/binary-aarch64/Packages',
}
# libxcb also carries the XCB extension libraries, headers and .pc files.
WANTED = ['libxcb', 'xorgproto', 'libx11', 'libxshmfence', 'libdrm']


def fetch(url, dest):
    if not dest.exists():
        with urllib.request.urlopen(url) as response, dest.open('wb') as out:
            shutil.copyfileobj(response, out)
    return dest


def parse_index(text):
    for block in text.split('\n\n'):
        fields = {}
        for line in block.splitlines():
            if line.startswith(' ') or ': ' not in line:
                continue
            key, value = line.split(': ', 1)
            fields.setdefault(key, value)
        if 'Package' in fields and 'Filename' in fields:
            yield fields


def main():
    if SYSROOT.exists():
        shutil.rmtree(SYSROOT)
    (SYSROOT / 'usr').mkdir(parents=True)
    debs = WORK / 'debs'
    debs.mkdir(exist_ok=True)
    scratch = WORK / 'deb-extract'
    missing = list(WANTED)
    for repo, url in REPOS.items():
        index = fetch(url, WORK / f'Packages-{repo}')
        for fields in parse_index(index.read_text(encoding='utf-8', errors='replace')):
            name = fields['Package']
            if name not in missing:
                continue
            base = 'https://packages.termux.dev/apt/'
            base += 'termux-x11/' if repo == 'termux-x11' else 'termux-main/'
            deb = fetch(base + fields['Filename'], debs / f'{name}.deb')
            # Termux .deb payloads are rooted at data/data/com.termux/files/usr.
            if scratch.exists():
                shutil.rmtree(scratch)
            scratch.mkdir()
            subprocess.run(['dpkg-deb', '-x', str(deb), str(scratch)], check=True)
            payload = scratch / 'data/data/com.termux/files/usr'
            if not payload.is_dir():
                raise SystemExit(f'unexpected .deb layout for {name}')
            subprocess.run(['cp', '-a', f'{payload}/.', str(SYSROOT / 'usr')], check=True)
            missing.remove(name)
            print(f'extracted {name} {fields.get("Version", "?")}', flush=True)
    shutil.rmtree(scratch, ignore_errors=True)
    if missing:
        raise SystemExit(f'missing Termux packages: {missing}')
    pkgconfig = SYSROOT / 'usr/lib/pkgconfig'
    # Termux .pc files hardcode their install prefix; relink them to sysroot.
    termux_prefix = '/data/data/com.termux/files/usr'
    for pc in pkgconfig.glob('*.pc'):
        text = pc.read_text()
        if termux_prefix in text:
            text = text.replace(termux_prefix, str(SYSROOT / 'usr'))
        kept = []
        for line in text.splitlines(keepends=True):
            if line.startswith(('Requires:', 'Requires.private:')):
                modules = line.split(':', 1)[1].split(',')
                if not all(f'{m.split()[0]}.pc' in
                           {p.name for p in pkgconfig.glob('*.pc')} for m in modules if m):
                    continue
            kept.append(line)
        pc.write_text(''.join(kept))
    for required in ('xcb.pc', 'xcb-dri3.pc', 'xcb-present.pc', 'xcb-sync.pc',
                     'xcb-randr.pc', 'xcb-shm.pc', 'xcb-dri2.pc', 'x11-xcb.pc',
                     'xshmfence.pc', 'libdrm.pc', 'xproto.pc', 'kbproto.pc'):
        if not (pkgconfig / required).is_file():
            raise SystemExit(f'missing pkg-config file: {required}')
    # xcb-keysyms is not part of the container's library set; never let it in.
    for unwanted in pkgconfig.glob('xcb-keysyms.pc'):
        unwanted.unlink()
    # The wrapper only calls XGetXCBConnection(), so keep libX11 out of
    # DT_NEEDED; the container ships libX11-xcb but not plain libX11.
    x11xcb = pkgconfig / 'x11-xcb.pc'
    text = x11xcb.read_text()
    patched = re.sub(r'^Requires:.*$', 'Requires: xcb', text, count=1, flags=re.M)
    if patched != text:
        x11xcb.write_text(patched)
    print('link sysroot ready:', SYSROOT)


if __name__ == '__main__':
    sys.exit(main())
