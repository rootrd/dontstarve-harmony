#!/usr/bin/env python3
"""Asset-independent host tests. Run in WSL/Linux; VULKAN_INC points to SDK usr/include."""
import os
from pathlib import Path
import shutil
import tempfile
import subprocess

from test_dst_assets import compile_run

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'vk-compat'


def test_vertex_bindings():
    # Exercise the real format/planning code on a Vulkan-minimum 16-binding device.
    source = (ROOT / 'dm/gfx/nvn/nvn_vulkan.c').read_text(encoding='utf-8')
    start = source.index('static const VkFormat VF8F')
    end = source.index('// ---- 管线缓存', start)
    code = r"""
#include <assert.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
#include "dm/gfx/nvn/nvn_obj.h"
#include "dm/gfx/nvn/nvn_shbin.h"
static struct {va_st attr[16];vs_st stream[16];int nattr,nstream;} S;
static shb_entry g_shb_e[1];
static shb_vi g_shb_vi[16];
""" + source[start:end] + r"""
int main(void){
 S.nattr=S.nstream=16;g_shb_e[0].nvi=16;
 for(int i=0;i<16;i++){
  g_shb_vi[i].loc=i;S.attr[i].stream=i;S.stream[i].stride=16;
 }
 // Every possible set of live streams, including binding 15 and all 16.
 for(unsigned mask=0;mask<65536;mask++){
  for(int i=0;i<16;i++)S.attr[i].fmt=(mask&(1u<<i))?0x2e:0;
  unsigned b=vtx_const_binding(0);
  if(mask==65535)assert(b==VTX_CONST_BINDING);
  else assert(b<16&&!(mask&(1u<<b)));
 }
 // A shrinking rebind must release the old tail's streams for fallback.
 for(int i=0;i<16;i++)S.attr[i].fmt=0x2e;
 S.nattr=15;assert(vtx_const_binding(0)==15);
 S.nattr=16;S.nstream=15;assert(vtx_const_binding(0)==15);
 puts("PASS: production vertex fallback avoids live streams for all 65536 binding masks");
}
"""
    with tempfile.TemporaryDirectory(prefix='dst-vtx-') as tmp:
        compile_run(Path(tmp), 'vertex_bindings', code, includes=(OUT / 'include', ROOT / 'dm/gfx/nvn/include'))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    inc = Path(os.environ.get('VULKAN_INC', '/usr/include'))
    if not (inc / 'vulkan/vulkan.h').is_file():
        raise SystemExit('Set VULKAN_INC to the directory containing vulkan/vulkan.h')
    for name in ('vulkan', 'vk_video'):
        if (inc / name).is_dir():
            shutil.copytree(inc / name, OUT / 'include' / name, dirs_exist_ok=True)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                    '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-I' + str(OUT / 'include'),
                    str(ROOT / 'dm/gfx/nvn/tests/vk_compat.c'),
                    str(ROOT / 'dm/gfx/tex/nvn_bcdec.c'),
                    str(ROOT / 'dm/gfx/tex/nvn_deswizzle.c'),
                    str(ROOT / 'dm/gfx/nvn/nvn_tex.c'), '-lm', '-o', str(OUT / 'vk_compat')], check=True)
    subprocess.run([str(OUT / 'vk_compat')], check=True)
    test_vertex_bindings()
    # This checkout stores shell scripts with CRLF. Keep the normalized copy
    # under build/, so the original runner's dirname/.. still resolves ROOT.
    runner = ROOT / 'build/pkg_faults.sh'
    runner.write_text((ROOT / 'scripts/pkg_faults.sh').read_text(), newline='\n')
    subprocess.run(['bash', str(runner)], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
