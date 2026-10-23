#!/usr/bin/env python3
"""Build a headless Vulkan texture/readback probe for an OHOS device.

The executable needs no game data. Retail devices may require execution through
a signed debug HAP even after ELF code signing. Device execution is an explicit
hdc step so building this probe never selects or changes a connected device.
"""
import argparse
from pathlib import Path
import struct
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ndk', type=Path, required=True)
    ap.add_argument('--glslang', default='glslangValidator')
    ap.add_argument('--spirv-val', default='spirv-val')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    work = root / 'build/vk-device'; work.mkdir(parents=True, exist_ok=True)
    header = '#include <stdint.h>\n'
    for name, sampler, position in [('2d', 'sampler2D', 'ivec2(0)'),
                                     ('array', 'sampler2DArray', 'ivec3(0,0,19)'),
                                     ('3d', 'sampler3D', 'ivec3(0,0,1)')]:
        src = work / (name + '.comp'); binary = src.with_suffix('.spv')
        src.write_text(f'''#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0) uniform {sampler} source_image;
layout(set=0,binding=1,std430) buffer Result {{ vec4 color; }} result;
void main() {{ result.color = texelFetch(source_image, {position}, 0); }}
''')
        subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.1', str(src), '-o', str(binary)], check=True)
        subprocess.run([args.spirv_val, '--target-env', 'vulkan1.1', str(binary)], check=True)
        words = struct.unpack('<' + 'I' * (binary.stat().st_size // 4), binary.read_bytes())
        header += f'static const uint32_t probe_{name}[] = {{\n'
        header += ',\n'.join(','.join(f'0x{w:08x}' for w in words[i:i+8]) for i in range(0, len(words), 8)) + '\n};\n'
    (work / 'vk_probe_shaders.h').write_text(header)
    output = work / 'vk-device'
    subprocess.run([str(args.ndk / 'llvm/bin/clang'), '--target=aarch64-linux-ohos',
                    '--sysroot=' + str(args.ndk / 'sysroot'), '-std=c11', '-O1', '-g', '-Wall', '-Werror',
                    '-Wno-missing-field-initializers', '-I' + str(work), '-I' + str(root / 'dm/gfx/nvn'),
                    '-I' + str(root / 'dm/gfx/tex'), str(root / 'dm/gfx/nvn/tests/vk_device.c'),
                    str(root / 'dm/gfx/tex/nvn_bcdec.c'), '-lvulkan', '-lm', '-o', str(output)], check=True)
    print(output)


if __name__ == '__main__':
    main()
