#!/usr/bin/env python3
"""Stage a local DST HAP project from extracted, patched, validated game assets.

Copyrighted assets and signing material stay under ignored build/, never hap/.
Run on Windows to create hard links on the same volume as the source assets.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil


def link_file(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        if os.path.samefile(src, dst):
            return
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', type=Path, default=Path('build/game'))
    ap.add_argument('--native', type=Path, default=Path('dm/ohos/build/libdmhost.so'))
    ap.add_argument('--output', type=Path, default=Path('build/hap'))
    a = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    game, native, out = [(root / p).resolve() for p in (a.game, a.native, a.output)]
    if not out.is_relative_to(root / 'build'):
        ap.error('output must be inside this repository build/ directory')
    rom = game / 'update/RomFS'
    if not (rom / 'prefabs.xml').is_file():
        ap.error('expected extracted DST RomFS (prefabs.xml missing)')
    for p in [native, game / 'shaders.bin']:
        if not p.is_file():
            ap.error(f'missing {p}')
    modules = ['main', 'sdk']
    suffixes = ['patched.bin', 'params.tsv', 'gates.tsv', 'sites.tsv',
                'fixups.tsv', 'obs.tsv', 'cuts.tsv']
    guests = [game / 'patched' / f'{m}.{s}' for m in modules for s in suffixes]
    for p in guests:
        if not p.is_file():
            ap.error(f'missing {p}')
    # Preserve local signing and incremental outputs when restaging the sources.
    for src in (root / 'hap').rglob('*'):
        rel = src.relative_to(root / 'hap')
        if any(part in {'.idea', '.hvigor', 'oh_modules', 'build'} for part in rel.parts):
            continue
        if src.is_file() and rel.as_posix() != 'build-profile.json5':
            dst = out / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
    profile = out / 'build-profile.json5'
    if not profile.exists():
        data = (root / 'hap/build-profile.json5').read_text(encoding='utf-8')
        data = json.loads(re.sub(r'//[^\n]*', '', data))
        for product in data['app']['products']:
            product.pop('signingConfig', None)
        profile.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    depfile = out / 'entry/oh-package.json5'
    deps = json.loads(depfile.read_text(encoding='utf-8'))
    deps['dependencies']['libdmhost.so'] = 'file:./src/main/cpp/types/libdmhost'
    depfile.write_text(json.dumps(deps, indent=2) + '\n', encoding='utf-8')
    link_file(native, out / 'entry/libs/arm64-v8a/libdmhost.so')
    raw = out / 'entry/src/main/resources/rawfile'
    digest = hashlib.sha256()
    for p in guests:
        link_file(p, raw / p.name)
        digest.update(p.name.encode())
        digest.update(p.read_bytes())
    (raw / 'guest-manifest.json').write_text(json.dumps({
        'version': digest.hexdigest(), 'modules': modules,
        'files': [p.name for p in guests]}, indent=2) + '\n', encoding='utf-8')
    link_file(game / 'shaders.bin', out / 'entry/src/main/resources/resfile/shaders.bin')
    # Keep both modules below the signed-HAP 4 GiB limit, without splitting files.
    # Every loose root file, including boot markers, is in the entry module.
    sizes = [native.stat().st_size + sum(p.stat().st_size for p in guests), 0]
    counts = [0, 0]
    rom_sizes = [0, 0]  # Assignment must not depend on native-library or guest-patch sizes.
    files = sorted((p for p in rom.rglob('*') if p.is_file()),
                   key=lambda p: (-p.stat().st_size, p.as_posix()))
    expected = [set(['shaders.bin']), set()]
    for p in files:
        rel = p.relative_to(rom)
        i = 0 if len(rel.parts) == 1 else (0 if rom_sizes[0] <= rom_sizes[1] else 1)
        module = ['entry', 'ds2'][i]
        link_file(p, out / module / 'src/main/resources/resfile/romfs' / rel)
        expected[i].add('romfs/' + rel.as_posix())
        sizes[i] += p.stat().st_size
        rom_sizes[i] += p.stat().st_size
        counts[i] += 1
    for i, module in enumerate(['entry', 'ds2']):
        base = out / module / 'src/main/resources/resfile'
        for p in base.rglob('*'):
            if p.is_file() and p.relative_to(base).as_posix() not in expected[i]:
                p.unlink()  # Only stale links in the validated staging directory.
        if sizes[i] >= 3_900_000_000:
            raise RuntimeError(f'{module} exceeds safe HAP size: {sizes[i]}')
    icon = ('<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" '
            'viewBox="0 0 256 256"><rect width="256" height="256" rx="48" '
            'fill="#24221f"/><path d="M128 32L160 108L222 124L168 156L180 224'
            'L128 182L76 224L88 156L34 124L96 108Z" fill="#e0c490"/></svg>')
    for name in ('layered_image', 'startIcon'):
        p = out / 'AppScope/resources/base/media' / f'{name}.svg'
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(icon, encoding='utf-8')
    summary = {'project': str(out), 'modules': modules,
               'romfs_files': counts, 'module_bytes': sizes,
               'guest_version': digest.hexdigest()}
    (out.parent / 'hap-stage.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
