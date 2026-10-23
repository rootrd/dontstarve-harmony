#!/usr/bin/env python3
"""Exercise DST packing, real shader lookup and large-BSS loader contracts on Linux.

Needs GCC, lz4 and capstone. Pass --shaders to check a locally generated library.
No copyrighted data or keys are required for the synthetic regression cases.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, start):
    begin = source.index(start)
    end = source.index('\n}', begin) + 2
    return source[begin:end]


def compile_run(work, name, code, args=(), expected=0, includes=()):
    src = work / (name + '.c'); exe = work / name
    src.write_text(code)
    subprocess.run(['gcc', '-std=gnu11', '-fsanitize=address,undefined', '-fno-pie',
                    '-no-pie', '-g', '-I' + str(ROOT), *('-I' + str(p) for p in includes),
                    str(src), '-o', str(exe)], check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
    run = subprocess.run([str(exe), *map(str, args)], env=env)
    assert run.returncode == expected, (name, run.returncode, expected)


def test_patcher(work):
    text = struct.pack('<6I', 0x14000002, 0x4000, 0xd4000021, 0xaa0003f2, 0xd53bd040, 0xd65f03c0)
    ro = bytearray(0x80)
    ro[:4] = b'MOD0'
    struct.pack_into('<i', ro, 16, 0x40)
    ro[0x40:0x44] = bytes([1, 3, 3, 3])  # Empty .eh_frame_hdr table.
    data = b'DATA' * 12 + b'!'
    for bss in (0x345, 0xc0000000):
        nso = bytearray(0x100)
        nso[:4] = b'NSO0'
        off = len(nso)
        for i, (memory, segment) in enumerate(zip((0, 0x4000, 0x8000), (text, ro, data))):
            struct.pack_into('<III', nso, 0x10 + i * 0x10, off, memory, len(segment))
            struct.pack_into('<I', nso, 0x60 + i * 4, len(segment))
            off += len(segment)
        struct.pack_into('<I', nso, 0x3c, bss)
        nso.extend(text + ro + data)
        src = work / 'synthetic.nso'; src.write_bytes(nso)
        out = work / str(bss)
        run = subprocess.run([sys.executable, str(ROOT / 'dm/tools/patch_nso.py'),
                              str(src), str(out), 'fixture'], capture_output=True, text=True)
        if run.returncode:
            raise AssertionError(run.stderr + run.stdout)
        image = (out / 'fixture.patched.bin').read_bytes()
        layout = {}
        for line in (out / 'fixture.fixups.tsv').read_text().splitlines():
            _, key, value = line.split()
            layout[key] = int(value, 0)
        assert image[0x4000:0x4000 + len(ro)] == ro
        assert image[0x8000:0x8000 + len(data)] == data
        key, patch = layout['KEY_PAGE_OFF'], layout['PATCH_OFF']
        if bss > 0x4000000:
            assert layout['PATCH_BELOW'] == 1 and layout['BSS_SIZE'] == bss
            assert layout['TAIL_OFF'] == -layout['TAIL_LEN']
            assert len(image) == 0x8000 + len(data) + layout['TAIL_LEN'] < 9 * 1024 * 1024
            key_file = 0x8000 + len(data) + key - layout['TAIL_OFF']
        else:
            key_file = key
            assert not layout.get('PATCH_BELOW')
            assert key >= 0x8000 + len(data) + bss
        assert not any(image[key_file:key_file + 0x4000])
        insn = struct.unpack_from('<I', image, 8)[0]
        assert insn >> 26 == 5  # Original SVC must branch to a reachable trampoline.
        delta = insn & 0x3ffffff
        if delta & 0x2000000:
            delta -= 0x4000000
        assert patch <= 8 + delta * 4 < patch + 0x10000
    print('PASS: small and 3 GiB BSS images, key-page padding, trampoline branch reach')


def test_loader(work):
    source = (ROOT / 'dm/host/loader.c').read_text()
    code = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef struct { uint8_t *base, *tail_va; uint64_t key_page_off, tail_off, tail_len,
  key_page_size; size_t size; int patch_below; unsigned key_idx[8]; char *name; } module_t;
static module_t g_mod[1]; static int g_nmod=1;
static uint64_t g_tsd_off=16; static unsigned g_keyres[2]={5,6},g_keyaspect=7,g_keyaspectui=8;
enum {KEY_KRES0=4, KEY_KRES1, KEY_KASPECT, KEY_KASPECTUI};
static unsigned key_value(unsigned i){return i+1;}
#define LOG(...) ((void)0)
#define die(...) exit(99)
#define PROT_READ 1
#define mprotect(p,n,f) 0
'''
    code += function(source, 'static void install_key_page(')
    code += '\n' + function(source, 'module_t *module_of(')
    code += r'''
int main(int argc,char **argv){
 module_t *m=g_mod; m->tail_len=0x800000; m->tail_off=0-m->tail_len;
 m->key_page_off=m->tail_off; m->key_page_size=0x4000; m->patch_below=1;
 m->tail_va=calloc(1,m->tail_len+0x10000); m->base=m->tail_va+m->tail_len; m->size=0x10000;
 for(int i=0;i<8;i++)m->key_idx[i]=i;
 if(argc>1)m->key_page_off=m->tail_off+m->tail_len-0x1000;
 install_key_page(m);
 for(int i=0;i<8;i++)assert(((uint64_t*)m->tail_va)[i]==16+(i+1)*8);
 assert(module_of(m->tail_va+0x4000)==m && module_of(m->base+4)==m);
 assert(!module_of(m->base+m->size));
 free(m->tail_va);return 0;
}
'''
    compile_run(work, 'loader_valid', code)
    compile_run(work, 'loader_reject', code, ['overflow'], 99)
    print('PASS: production loader negative offsets, key-page bounds and trampoline ownership')


def test_shader_library(work, library):
    source = (ROOT / 'dm/gfx/nvn/nvn_vulkan.c').read_text()
    code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dm/gfx/nvn/nvn_shbin.h"
#define GVL(...) ((void)0)
#define GRED(...) ((void)0)
static int nvn_range_valid(uint64_t a,uint64_t n,uint64_t cap){return a<=cap&&n<=cap-a;}
'''
    code += source[source.index('#define SHB_MAXE'):source.index('// ---- 每段一个 VkShaderModule')]
    code += r'''
int main(int argc,char **argv){
 assert(argc==2);setenv("DM_SHBIN",argv[1],1);assert(shb_load());assert(g_shb_n>0);
 for(int i=0;i<g_shb_n;i++){
  const shb_entry *e=&g_shb_e[i];char key[17];
  for(int b=0;b<8;b++)sprintf(key+b*2,"%02x",e->key[b]);
  assert(shb_get(hexk(key),e->stage)==i && shb_has(hexk(key)));
  assert(e->spv_words>=5 && g_shb_spv[e->spv_off]==0x07230203);
 }
 printf("PASS: production shader loader and lookup, %d stages\n",g_shb_n);return 0;
}
'''
    compile_run(work, 'shader_loader', code, [library.resolve()])



def test_romfs_paths(work):
    source = (ROOT / 'dm/host/hle_fs.c').read_text()
    rom = work / 'romfs'; (rom / 'scripts').mkdir(parents=True)
    (rom / 'scripts/real.lua').write_text('return true')
    (rom / 'build.config').write_text('config')
    (rom / '.actual').write_text('hidden')
    (rom / 'actual').write_text('visible')
    code = r"""
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define __OHOS__ 1
static size_t strlcpy(char *d,const char *s,size_t n){
 size_t k=strlen(s);if(n){size_t j=k<n-1?k:n-1;memcpy(d,s,j);d[j]=0;}return k;
}
"""
    # No path_component stub: the OHOS read-only resolver must not call the
    # directory/xattr scanner, including on a missing overlay file.
    code += function(source, 'static int root_path(')
    code += r"""
int main(int argc,char **argv){
 assert(argc==2);char out[1024],expected[1024];const char *root=argv[1];
 assert(root_path(root,"//scripts/./real.lua",out,sizeof out));
 snprintf(expected,sizeof expected,"%s/scripts/real.lua",root);assert(!strcmp(out,expected));
 assert(!root_path(root,"scripts/Real.lua",out,sizeof out)&&errno==ENOENT);
 assert(!root_path(root,"scripts/missing.lua",out,sizeof out)&&errno==ENOENT);
 assert(!root_path(root,"scripts/../build.config",out,sizeof out));
 assert(root_path(root,".build.config",out,sizeof out));
 snprintf(expected,sizeof expected,"%s/build.config",root);assert(!strcmp(out,expected));
 assert(root_path(root,".actual",out,sizeof out));
 snprintf(expected,sizeof expected,"%s/.actual",root);assert(!strcmp(out,expected));
 assert(!root_path(root,"scripts/real.lua",out,strlen(root)+5)&&errno==ENAMETOOLONG);
 assert(!root_path(root,"",out,2)&&errno==ENAMETOOLONG);
 assert(!root_path(root,"",out,0)&&errno==ENAMETOOLONG);
 puts("PASS: production OHOS RomFS exact paths, split misses, dot files and bounds");return 0;
}
"""
    compile_run(work, 'romfs_paths', code, [rom])



def test_arena_reuse(work):
    source = (ROOT / 'dm/gfx/nvn/nvn_vulkan.c').read_text()
    code = r"""
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARENA_SZ (1u<<20)
#define ARENA_GUARD 4096u
static uint8_t *g_arena_map;
static uint64_t g_arena_off, g_arena_gen=1, g_arena_stamp;
typedef struct {uint64_t addr,len,gen,stamp;uint32_t off;} adedup;
static adedup g_ahash[8192];
static _Atomic uint64_t g_adup_hit,g_adup_new,g_adup_bad,g_arena_full,g_adup_dirty;
static int g_arena_nodup,g_ahit,g_skip;
static int mtl_addr(uint64_t a,uint64_t n,void **p,uint64_t *off){
 (void)n;*p=(void *)(uintptr_t)a;*off=0;return a!=0;
}
static uint64_t nvn_dirty_now(void){return 1;}
static int nvn_dirty_tracked(const void *p,size_t n){(void)p;(void)n;return 1;}
static int nvn_dirty_since(const void *p,size_t n,uint64_t stamp){(void)p;(void)n;(void)stamp;return 0;}
static void arena_arm(void *p,size_t n){(void)p;(void)n;}
static struct {uint64_t gen,addr,len,stamp;uint32_t off;int ok;} g_vtx[16];
static _Atomic uint64_t g_vtx_dirty,g_vtx_new,g_vap_patch;
static void vtx_check(uint64_t a,uint64_t l,uint32_t o){(void)a;(void)l;(void)o;}
static struct {
 struct {uint64_t address,size;} vbuf[16];
 struct {uint32_t stride,divisor;} stream[16];
 struct {int fmt,stream;uint32_t offset;} attr[16];int nattr;
} S;
"""
    for fn in ('static int arena_rewritten(', 'static uint64_t arena_put(', 'static int vtx_stream('):
        code += function(source, fn)
    code += r"""
int main(void){
 g_arena_map=calloc(1,ARENA_SZ);assert(g_arena_map);
 uint8_t guest[12288];memset(guest,3,sizeof guest);
 uint64_t addr=(uint64_t)(uintptr_t)guest;uint32_t first,next;uint64_t used;
 assert(arena_put(addr,sizeof guest,0,&first));used=g_arena_off;
 assert(arena_put(addr,sizeof guest,0,&next)&&next==first&&g_arena_off==used);
 // Tracking is registered but its stamp never advances, as with unarmed pages.
 guest[8197]=77;
 assert(arena_put(addr,sizeof guest,0,&next)&&next!=first);
 assert(g_arena_map[first+8197]==3&&g_arena_map[next+8197]==77);
 first=next;used=g_arena_off;
 assert(arena_put(addr,sizeof guest,0,&next)&&next==first&&g_arena_off==used);
 // Exercise the separate per-stream cache, then return to the arena hash route.
 S.vbuf[0].address=addr;S.vbuf[0].size=sizeof guest;
 assert(vtx_stream(0,&first));guest[10001]=91;
 assert(vtx_stream(0,&next)&&next!=first);
 assert(g_arena_map[first+10001]==3&&g_arena_map[next+10001]==91);
 first=next;used=g_arena_off;
 assert(vtx_stream(0,&next)&&next==first&&g_arena_off==used);
 // A short index list is also mutable without rebinding its address or size.
 uint16_t indices[]={0,1,2,2,3,0};addr=(uint64_t)(uintptr_t)indices;
 assert(arena_put(addr,sizeof indices,0,&first));indices[4]=42;
 assert(arena_put(addr,sizeof indices,0,&next)&&next!=first);
 assert(!(first%16)&&!(next%16));
 assert(((uint16_t *)(g_arena_map+first))[4]==3&&((uint16_t *)(g_arena_map+next))[4]==42);
 assert(!arena_put(addr,UINT64_MAX,0,&next));
 assert(!arena_put(addr,sizeof indices,UINT64_MAX,&next));
 assert(arena_rewritten(guest,32,ARENA_SZ));
 free(g_arena_map);puts("PASS: production arena and vertex caches align buffers and detect full-range rewrites with unchanged dirty stamps");return 0;
}
"""
    compile_run(work, 'arena_reuse', code)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--shaders', type=Path)
    args = ap.parse_args()
    spec = importlib.util.spec_from_file_location('builder', ROOT / 'scripts/build_dst_shaders.py')
    builder = importlib.util.module_from_spec(spec); spec.loader.exec_module(builder)
    for handle, slot in ((8, 0), (10, 1), (18, 5), (70, 31)):
        row = builder.texture_binding(dict(array=1, separate=False, cbuf_slot=-1,
                                           handle=handle, type=3, binding=9))
        assert row[3:5] == (slot + 1, slot)
    for bad in (0, 7, 9, 72):
        try:
            builder.texture_binding(dict(array=1, separate=False, cbuf_slot=-1,
                                         handle=bad, type=3, binding=9))
        except ValueError:
            pass
        else:
            raise AssertionError('accepted unsupported combined handle')
    with tempfile.TemporaryDirectory(prefix='dst-assets-') as tmp:
        work = Path(tmp)
        test_patcher(work)
        test_loader(work)
        test_romfs_paths(work)
        test_arena_reuse(work)
        if args.shaders:
            test_shader_library(work, args.shaders)
    print('All DST asset regressions passed')


if __name__ == '__main__':
    main()
