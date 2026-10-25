#!/usr/bin/env python3
"""Install both signed DST modules with hdc/bm, retaining the app's saves."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import zipfile


def inspect_haps(project):
    packages = []
    bundle = version = None
    for module in ('entry', 'ds2'):
        path = project / module / 'build/default/outputs/default' / f'{module}-default-signed.hap'
        with zipfile.ZipFile(path) as archive:
            info = json.loads(archive.read('module.json'))
            app = info['app']
            if info['module']['name'] != module:
                raise ValueError(f'{path}: wrong module')
            if bundle is None:
                bundle, version = app['bundleName'], app['versionCode']
            if (bundle, version) != (app['bundleName'], app['versionCode']):
                raise ValueError('entry/ds2 bundle or version mismatch')
            resources = [f for f in archive.infolist() if f.filename.startswith('resources/resfile/romfs/') and not f.is_dir()]
            if not resources:
                raise ValueError(f'{module}: RomFS missing')
            print(f'{module}: {path.stat().st_size} bytes, {len(resources)} resource files', flush=True)
        packages.append((module, path))
    return bundle, packages


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--project', type=Path, default=Path('build/hap'))
    ap.add_argument('--target', required=True, help='Explicit hdc target from hdc list targets')
    ap.add_argument('--hdc', default='hdc')
    ap.add_argument('--check-only', action='store_true', help='Inspect local packages without changing the device')
    args = ap.parse_args()
    bundle, packages = inspect_haps(args.project.resolve())
    if args.check_only:
        return

    def hdc(*argv, timeout=60):
        result = subprocess.run([args.hdc, '-t', args.target, *argv], capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=timeout, check=True)
        output = result.stdout + result.stderr
        if '[Fail]' in output or '[Error]' in output:
            raise RuntimeError(output)
        return output

    def shell(*argv, timeout=60):
        return hdc('shell', shlex.join(argv), timeout=timeout)

    remote_dir = '/data/local/tmp/dstapp'
    shell('mkdir', '-p', remote_dir)
    remote_paths = []
    for module, path in packages:
        digest = hashlib.sha256()
        with path.open('rb') as f:
            for chunk in iter(lambda: f.read(8 << 20), b''):
                digest.update(chunk)
        expected = digest.hexdigest()
        remote = f'{remote_dir}/{module}.hap'
        actual = shell('sha256sum', remote, timeout=300).split()
        if not actual or actual[0] != expected:
            print(f'Transferring {module}...', flush=True)
            print(hdc('file', 'send', str(path), remote, timeout=900).strip(), flush=True)
            actual = shell('sha256sum', remote, timeout=300).split()
        if not actual or actual[0] != expected:
            raise RuntimeError(f'{module}: transferred HAP SHA-256 mismatch; refusing installation')
        print(f'{module}: SHA-256 verified', flush=True)
        remote_paths.append(remote)
    # A single transaction installs entry and its resource feature. Never uninstall:
    # replacing a signed build preserves saves and does not require a fresh sandbox.
    result = shell('bm', 'install', '-r', '-p', *remote_paths, timeout=600)
    print(result.strip(), flush=True)
    if not re.search(r'install bundle successfully', result, re.I):
        raise RuntimeError('Package manager did not report installation success')
    state = shell('bm', 'dump', '-n', bundle)
    info = json.loads(state[state.index('{'):])
    modules = {m['moduleName'] for m in info['hapModuleInfos']}
    if not {'entry', 'ds2'} <= modules:
        raise RuntimeError(f'Installed module verification failed: {sorted(modules)}')
    print(f'Installed {bundle}: entry + ds2; existing app data retained.')


if __name__ == '__main__':
    main()
