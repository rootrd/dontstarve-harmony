#!/usr/bin/env python3
"""Compile exact production logger/trace/SHB functions with host-only platform stubs.
No game data, SDK, device, network, or Vulkan calls. Does not test GPU rendering.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'nvn/nvn_vulkan.c').read_text()
headers = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nvn_diag.h"
#include "nvn_shbin.h"
#define GVL(...) ((void)0)
#define GRED(...) ((void)0)
'''
logger = source[source.index('static FILE *g_lf;'):source.index('#define GLOG_MAX')]
trace = source[source.index('static pthread_once_t g_textrace_once'):source.index('static vktex g_tex[VTN];')]
shader = source[source.index('#define SHB_MAXE'):source.index('// ---- 每段一个 VkShaderModule')]
body = headers + logger + r'''
typedef struct { uint64_t serial; void *img; int uploaded, drawn, pkg_bad, pkg_tried; } vktex;
static uint64_t g_presents;
''' + trace + shader + r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    // The production logger must truncate a long formatted message safely.
    g_lf = tmpfile(); assert(g_lf);
    dm_logfmt("%4000s", "x");
    assert(ftell(g_lf) == 1200); // 1199 payload bytes + newline
    fclose(g_lf); g_lf = NULL;
    setenv("DM_VK_TEXTRACE", "1", 1);
    if (!strcmp(argv[1], "filter")) setenv("DM_VK_TEXTRACE_SERIAL", "7", 1);
    vktex a = {7, (void *)(uintptr_t)1, 0, 0, 0, 0};
    vktex b = {7, (void *)(uintptr_t)2, 0, 0, 0, 0};
    textrace(NVN_TRACE_QUEUE, &a, -1, "accepted");
    textrace(NVN_TRACE_QUEUE, &b, -1, "accepted");
    assert(g_textrace_budget.emitted[NVN_TRACE_QUEUE] == 2); // siblings distinct
    textrace(NVN_TRACE_QUEUE, &a, -1, "accepted");
    assert(g_textrace_budget.emitted[NVN_TRACE_QUEUE] == 2);
    if (!strcmp(argv[1], "filter")) {
        b.serial = 8; textrace(NVN_TRACE_QUEUE, &b, -1, "accepted");
        assert(g_textrace_budget.emitted[NVN_TRACE_QUEUE] == 2);
    }
    g_presents = 6000;
    textrace(NVN_TRACE_QUEUE, &a, -1, "accepted");
    assert(g_textrace_budget.emitted[NVN_TRACE_QUEUE] == 1);

    char path[] = "/tmp/nvn_shb_test_XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    shb_hdr h = {.magic=SHB_MAGIC, .ver=SHB_VER, .nentries=2};
    shb_entry e[2] = {0};
    e[0].key[0] = 1; e[1].key[0] = 2; e[1].stage = 1;
    int invalid = !strcmp(argv[1], "invalid");
    if (invalid) { e[1].spv_off=UINT32_MAX; e[1].spv_words=1; }
    assert(write(fd, &h, sizeof h) == sizeof h);
    assert(write(fd, e, sizeof e) == sizeof e); close(fd);
    setenv("DM_SHBIN", path, 1);
    assert(shb_load() == !invalid); unlink(path);
    if (invalid) {
        assert(!g_shb_ready && g_shb_fail && !g_shb_map && !g_shb_e);
        assert(shb_get(k8v(e[0].key), 0) == -1);
        assert(!shb_has(k8v(e[0].key)));
        int vi=99, fi=99;
        shb_pair("0100000000000000", "0200000000000000", &vi, &fi);
        assert(vi == -1 && fi == -1);
        shb_stage("0100000000000000", &vi, &fi);
        assert(vi == -1 && fi == -1);
    } else {
        assert(shb_get(k8v(e[0].key), 0) == 0);
        assert(shb_get(k8v(e[1].key), 1) == 1);
        assert(shb_has(k8v(e[0].key)));
        munmap(g_shb_map, sizeof h + sizeof e);
    }
    puts("PASS: exact production logger, trace, and shader loader");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nvn_diag_test_') as td:
    src, exe = Path(td)/'integration.c', Path(td)/'integration'
    src.write_text(body)
    # Stubs deliberately discard log-only parameters; inherited hexk formatting
    # triggers GCC misleading-indentation. Those warnings do not hide new errors.
    cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-function', '-Wno-unused-variable', '-Wno-misleading-indentation',
           '-pthread', '-I'+str(root/'nvn'), str(src), '-o', str(exe)]
    if os.environ.get('SANITIZE') == '1':
        cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    subprocess.run(cmd, check=True)
    for mode in ['valid', 'invalid', 'filter']:
        subprocess.run([str(exe), mode], check=True)
