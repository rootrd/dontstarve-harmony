#!/usr/bin/env python3
"""Build DST shaders_nx/*.ksh into the Vulkan replay library (no game data in Git).

Requires the repository's dmsh_xlate, .NET 10, spirv-cross and spirv-val.
Each descriptor is mapped from translator metadata; unsupported layouts fail closed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

CONTROL_SIZE = 0x838  # Must match CTRL_STABLE / KEY_CODE_MAX in nvn_vulkan.c.
MAGIC = bytes.fromhex('34127698')


def extract(directory, work):
    jobs, sources = {}, {}
    files = sorted(directory.glob('*.ksh'))
    if not files:
        raise ValueError(f'No shaders_nx/*.ksh under {directory}')
    for path in files:
        data = path.read_bytes()
        controls = [m.start() for m in re.finditer(MAGIC, data)]
        if not controls:
            raise ValueError(f'{path.name}: no NVN control block; check extraction integrity')
        for off in controls:
            ctl = data[off:off + CONTROL_SIZE]
            if len(ctl) != CONTROL_SIZE:
                raise ValueError(f'{path.name}: truncated control block')
            c1off, total = struct.unpack_from('<II', ctl, 0x700)
            if not 0x80 <= c1off <= total <= 0x20000:
                raise ValueError(f'{path.name}: invalid code / constant range')
            start = off + CONTROL_SIZE
            code = data[start:start + total]
            if len(code) != total or code[:4] != bytes.fromhex('78563412'):
                raise ValueError(f'{path.name}: invalid graphics code block')
            stage = {1: 'vs', 5: 'ps'}.get((struct.unpack_from('<I', code, 0x30)[0] >> 10) & 15)
            if stage is None:
                raise ValueError(f'{path.name}: unsupported shader stage')
            key = hashlib.sha256(ctl + code[:CONTROL_SIZE]).hexdigest()[:16]
            source = {'file': path.name, 'offset': off, 'stage': stage}
            sources.setdefault(key, []).append(source)
            target = work / (key + '.code')
            if key in jobs and target.read_bytes() != code[0x30:]:
                raise ValueError(f'{path.name}: key collision with different code')
            target.write_bytes(code[0x30:])  # Ryujinx begins at SPH, after the NVN header.
            jobs[key] = dict(key=key, stage=stage, code=str(target),
                             c1=code[c1off:total].hex(), debug=False, reduced=False)
    return list(jobs.values()), sources


def texture_binding(t):
    # DST uses the combined c[2] table at byte 0x20, eight bytes per handle.
    # D2R's separate tables at 0x168 / 0x568 have a different ABI.
    if t['array'] != 1 or t['separate'] or t['cbuf_slot'] not in (-1, 2):
        raise ValueError(f'Unsupported texture handle layout at binding {t["binding"]}')
    handle = t['handle']
    if not 8 <= handle < 72 or (handle - 8) % 2:
        raise ValueError(f'Unexpected combined texture offset: {handle}')
    if t['type'] != 3:  # SamplerType.Texture2D; reject rather than silently lose array/shadow flags.
        raise ValueError(f'Unsupported texture dimension: {t["type"]}')
    reg = (handle - 8) // 2
    return (t['binding'], 1, 5, reg + 1, reg, 0, 0)


def pack(jobs, reflections, resources, binaries):
    entries, binds, vis, constants, spv = [], [], [], bytearray(), bytearray()
    audit = []
    if not 0 < len(jobs) <= 512:
        raise ValueError('Shader count exceeds SHB_MAXE')
    for job in sorted(jobs, key=lambda j: j['key']):
        key = job['key']; rf = reflections[key]; res = resources[key]
        if res['notes']:
            raise ValueError(f'{key}: unresolved translation queries: {res["notes"]}')
        if res['sbufs'] or res['images'] or any(rf.get(k) for k in
                ('ssbos', 'images', 'separate_images', 'separate_samplers', 'push_constants')):
            raise ValueError(f'{key}: unsupported descriptor category')
        cb = {b['binding']: b['slot'] for b in res['cbufs']}
        rows = []
        for u in rf.get('ubos', []):
            binding = u['binding']
            if u.get('set', 0) != 0 or u.get('array'):
                raise ValueError(f'{key}: unsupported UBO layout')
            if u['name'] == 'support_buffer' and binding == 0:
                rows.append((binding, 0, 0, 0, -1, 0, 0))
            elif binding in cb:
                slot = cb[binding]
                if 3 <= slot <= 16:
                    rows.append((binding, 0, 1, 0, slot - 3, 0, 0))
                elif slot in (0, 1, 2):
                    rows.append((binding, 0, 2 if slot == 1 else 3, slot, -1, 0, 0))
                else:
                    raise ValueError(f'{key}: unsupported constant bank {slot}')
            else:
                raise ValueError(f'{key}: UBO reflection disagrees with translator')
        tex = {t['binding']: t for t in res['textures']}
        for t in rf.get('textures', []):
            if t.get('set', 0) != 0 or t.get('array') or t['type'] != 'sampler2D':
                raise ValueError(f'{key}: unsupported sampled image layout')
            rows.append(texture_binding(tex.pop(t['binding'])))
        if tex or len(rows) != len({r[0] for r in rows}) or len(rows) > 64:
            raise ValueError(f'{key}: inconsistent descriptor reflection')
        inputs = []
        if job['stage'] == 'vs':
            for v in rf.get('inputs', []):
                ty = v['type']; cls = 1 if ty.startswith(('uint', 'uvec')) else 2 if ty.startswith(('int', 'ivec')) else 0
                inputs.append((v['location'], cls, 0, 0))
        c1 = bytes.fromhex(job['c1']); code = binaries[key]
        if len(code) % 4 or code[:4] != bytes.fromhex('03022307'):
            raise ValueError(f'{key}: invalid SPIR-V')
        entries.append(struct.pack('<8s8B6I', bytes.fromhex(key),
            0 if job['stage'] == 'vs' else 1, len(rows), len(inputs), 0, 0, 0, 0, 0,
            len(binds), len(vis), len(spv) // 4, len(code) // 4, len(constants), len(c1)))
        binds.extend(rows); vis.extend(inputs); constants.extend(c1)
        constants.extend(bytes(-len(constants) % 4)); spv.extend(code)
        audit.append(dict(key=key, stage=job['stage'], bindings=rows, vertex_inputs=inputs,
                          c1_bytes=len(c1), spirv_sha256=hashlib.sha256(code).hexdigest()))
    header = struct.pack('<8I', 0x534b5644, 1, len(entries), len(binds), len(vis), len(constants), len(spv), 0)
    data = header + b''.join(entries) + b''.join(struct.pack('<BBBBhBB', *b) for b in binds)
    data += b''.join(struct.pack('<4B', *v) for v in vis) + constants + spv
    return data, audit


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--dotnet', default='dotnet')
    ap.add_argument('--spirv-cross', default='spirv-cross')
    ap.add_argument('--spirv-val', default='spirv-val')
    args = ap.parse_args()
    work = args.output.resolve().parent / 'shader-build'
    work.mkdir(parents=True, exist_ok=True)
    jobs, sources = extract(args.romfs / 'shaders_nx', work)
    jobs_file = work / 'jobs.json'; jobs_file.write_text(json.dumps(jobs, indent=2))
    subprocess.run([args.dotnet, str(args.translator.resolve()), str(jobs_file), str(work)], check=True)
    reflections, resources, binaries = {}, {}, {}
    for job in jobs:
        key = job['key']; code = work / (key + '.spv')
        subprocess.run([args.spirv_val, '--target-env', 'vulkan1.1', str(code)], check=True)
        result = subprocess.run([args.spirv_cross, str(code), '--reflect'], check=True, capture_output=True, text=True)
        reflections[key] = json.loads(result.stdout)
        (work / (key + '.reflect.json')).write_text(result.stdout)
        resources[key] = json.loads((work / (key + '.res.json')).read_text())
        binaries[key] = code.read_bytes()
    data, audit = pack(jobs, reflections, resources, binaries)
    temporary = args.output.with_suffix('.tmp'); temporary.write_bytes(data); temporary.replace(args.output)
    (work / 'manifest.json').write_text(json.dumps(dict(
        key_recipe='sha256(control[:0x838] + code[:min(total,0x838)])[:8]',
        library_sha256=hashlib.sha256(data).hexdigest(), sources=sources, entries=audit), indent=2))
    print(f'{len(sources)} shaders validated and packed: {args.output} ({len(data)} bytes)')


if __name__ == '__main__':
    main()
