// 多模块装载（扮演 Horizon 的 rtld）：映射、占线程槽、门登记、跨模块重定位、按名接管、段权限。
// 代码区（原 text 与补丁区）一拷进来就设成只读+执行，此后装载器只写 data：iOS 上这两块要作为签名代码段原样内嵌。
// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
#include "dm.h"
#include <TargetConditionals.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "hle.h"

module_t g_mod[MAX_MOD];
int g_nmod;
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
#ifndef __APPLE__
static void tsd_calibrate(void);   // 前向声明：create_keys 里先调用 | forward declaration: called first from create_keys
#endif
pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;   // g_keyaspect：宽高比上限参数位（KASPECT）          // 分辨率参数位（改写器 PARAMS，KRES0 掌机 / KRES1 主机）；产物没有即 0、不占 | g_keyaspect: aspect ratio upper-limit parameter slot (KASPECT)          // resolution parameter slots (rewriter PARAMS, KRES0 handheld / KRES1 docked); 0 and not allocated if the build output lacks them
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_KRES0, KEY_KRES1, KEY_KASPECT, KEY_KASPECTUI, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    case KEY_KGATE: return g_keygate; case KEY_KRES0: return g_keyres[0]; case KEY_KRES1: return g_keyres[1];
    case KEY_KASPECT: return g_keyaspect; default: return g_keyaspectui;
    }
}
static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("创建宿主键失败");
    // 参数位键一律建（不按产物有没有用到判断：iOS 包里没有 params.tsv，按文件判断会漏建、键页写成 0 → 游戏读到 TSD 0 号槽）
    // Parameter slot keys are always created (not decided by whether the build output uses them: an iOS app has no params.tsv, so deciding by file would miss creating them and the key page would be written as 0 → the game reads TSD slot 0)
    if (pthread_key_create(&g_keyres[0], NULL) || pthread_key_create(&g_keyres[1], NULL) ||
        pthread_key_create(&g_keyaspect, NULL) || pthread_key_create(&g_keyaspectui, NULL)) die("创建参数位键失败");
    LOG("键号 K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu KRES0=%lu KRES1=%lu KASPECT=%lu KASPECTUI=%lu",
        g_key18, g_keytls, g_keytlr, g_keygate, g_keyres[0], g_keyres[1], g_keyaspect, g_keyaspectui);
#ifndef __APPLE__
    tsd_calibrate();   // musl：定位 tsd 数组偏移（键页偏移与槽基址都靠它） | musl: locate the tsd array offset (key-page offsets and the slot base depend on it)
#endif
}

// 槽基址：Apple 用 tpidrro_el0（其 TSD 槽 = tpidrro + 8*key）；Linux/musl（OHOS）用 tpidr_el0 + tsd 数组偏移。
static uint64_t g_tsd_off;   // musl：tsd 数组相对 tpidr_el0 的字节偏移；Apple = 0

static inline uint64_t slot_base(void) {
#ifdef __APPLE__
    uint64_t v; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v)); return v;
#else
    uint64_t v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v;
#endif
}

#ifndef __APPLE__
// musl 标定：建两只新键写可识别标记，正向扫描 tpidr_el0 近端内存（TLS 块必映射）定位 tsd 数组
static void tsd_calibrate(void) {
    static uint64_t m1 = 0x7ea5d0c4a11ce001ull, m2 = 0x7ea5d0c4a11ce002ull;
    pthread_key_t k1, k2;
    if (pthread_key_create(&k1, NULL) || pthread_key_create(&k2, NULL)) return;
    pthread_setspecific(k1, &m1);
    pthread_setspecific(k2, &m2);
    uint64_t tp = slot_base();
    for (uint64_t d = 0; d < 0x8000; d += 8) {
        uint64_t v = *(volatile uint64_t *)(tp + d);
        if (v == (uint64_t)(uintptr_t)&m1) {
            uint64_t v2 = *(volatile uint64_t *)(tp + d + (uint64_t)(k2 - k1) * 8);
            if (v2 == (uint64_t)(uintptr_t)&m2) g_tsd_off = d - (uint64_t)k1 * 8;
            break;
        }
    }
    pthread_setspecific(k1, NULL);
    pthread_setspecific(k2, NULL);
    LOG("TSD 标定（musl）：tsd 偏移 = %#llx（槽地址 = tpidr_el0 + %#llx + 8*key）",
        (unsigned long long)g_tsd_off, (unsigned long long)g_tsd_off);
}
#endif

// 键页取址：巨型 bss 时（patch_below）它在基址之下的尾部里，不是 base+key_page_off
// Key page address: with a giant bss (patch_below) it lives in the tail below the base, not at base+key_page_off
static uint8_t *key_page_of(module_t *m) {
    if (m->patch_below) return m->tail_va + (m->key_page_off - m->tail_off);
    return m->base + m->key_page_off;
}

// K 槽校验：槽 = 槽基址 + 键页偏移（Apple：tpidrro_el0 + 8*key；musl：tpidr_el0 + tsd_off + 8*key）
// K slot check: slot = slot base + key-page offset (Apple: tpidrro_el0 + 8*key; musl: tpidr_el0 + tsd_off + 8*key)
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base = slot_base();
    {
        uint8_t *kp = key_page_of(&g_mod[0]);
        uint8_t *lo = g_mod[0].patch_below ? g_mod[0].tail_va : g_mod[0].base;
        uint64_t span = g_mod[0].patch_below ? (g_mod[0].tail_len + g_mod[0].size) : g_mod[0].size;
        LOG("[tsd] m0=%s patch_below=%d base=%p tail_va=%p tail_off=%#llx key_page_off=%#llx kp=%p lo=%p span=%#llx size=%#zx idx18=%u",
            g_mod[0].name, g_mod[0].patch_below, (void *)g_mod[0].base, (void *)g_mod[0].tail_va,
            (unsigned long long)g_mod[0].tail_off, (unsigned long long)g_mod[0].key_page_off, (void *)kp,
            (void *)lo, (unsigned long long)span, g_mod[0].size, g_mod[0].key_idx[KEY_K18]);
        if (kp < lo || kp + 8 > lo + span) die("键页出映射外：kp 不在 [%p,%p)（patch_below=%d）", (void *)lo, (void *)(lo + span), g_mod[0].patch_below);
        uint64_t off = *(uint64_t *)(kp + g_mod[0].key_idx[KEY_K18] * 8);
        LOG("[tsd] off=%#llx slotbase=%p slot=%p", (unsigned long long)off, (void *)(base & ~7ull),
            (void *)(((uint64_t *)(base & ~7ull)) + off / 8));
        if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die("TSD 布局不符");
    }
    pthread_setspecific(g_key18, 0);
}

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// 地址 → 符号名（崩溃现场用：main+0x45f9d0 这种偏移人读不出是什么）。
// 先找「区间包住它」的（有 st_size 的优先），没有再取不超过它的最大 st_value —— 后者给的是最近的前一个符号，
// 对没进符号表的局部函数也能给出「某某+偏移」级别的线索。
// 只读模块自带的 .dynsym/.dynstr（都是映射好的页）、不分配不加锁 ⇒ 信号处理上下文里可用。
// Address → symbol name (for crash dumps: an offset like main+0x45f9d0 carries no readable name).
// First look for one whose range contains it (preferring symbols with st_size); failing that take the largest st_value not above it --
// the latter gives the nearest preceding symbol, which still hints at what a local function not in the symbol table sits next to.
// Reads only the module's own .dynsym/.dynstr (already mapped pages), allocates nothing and takes no lock => usable in a signal handler.
const char *module_sym_at(module_t *m, uint64_t a) {
    if (!m->symtab || !m->strtab || !m->nsym) return NULL;
    const Elf64_Sym *hit = NULL, *near = NULL;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (!s->st_value) continue;
        uint64_t lo = (uint64_t)m->base + s->st_value;
        if (a < lo) continue;
        if (s->st_size) { if (a - lo < s->st_size && (!hit || lo > (uint64_t)m->base + hit->st_value)) hit = s; }
        else if (!near || lo > (uint64_t)m->base + near->st_value) near = s;
    }
    const Elf64_Sym *r = hit ? hit : near;
    return r && r->st_name ? m->strtab + r->st_name : NULL;
}
// 符号名哈希表：按下标升序插入、同名已在则不覆盖 ⇒ 查询结果与线性扫描（首个已定义同名符号）逐条相同。
// ① 宿主装载 91–99 ms 里重定位 + 强制接管占 77–86 ms，主因是每条按名解析的重定位都线性扫两个模块的符号表
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // 同名已有更小下标：保留它 | The same name already exists with a smaller index: keep that one
        }
    }
}
// 模块内地址是否落在可执行区间：与装载时封页的两段一致 —— [base, base+round(text_end)) 加补丁区
// （巨型 bss 的补丁区映射在基址之下，所以按 tail_va 算）。崩溃现场靠它把「跳进数据段」和「跳进代码」分开。
// Whether an address inside a module lies in an executable range: same two segments the loader seals -- [base, base+round(text_end))
// plus the patch area (with a giant bss the patch area is mapped below the base, so it is computed from tail_va). The crash scene
// uses it to tell "jumped into data" apart from "jumped into code".
int module_addr_exec(const void *p) {
    module_t *m = module_of(p);
    if (!m) return 0;
    const uint8_t *a = (const uint8_t *)p;
    if (a >= m->base && a < m->base + ((m->text_end + 0x3FFF) & ~0x3FFFull)) return 1;
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (m->patch_below) {
        if (!m->tail_va || po < m->tail_off) return 0;
        const uint8_t *lo = m->tail_va + (po - m->tail_off);
        return a >= lo && a < lo + (m->tail_len - (po - m->tail_off));
    }
    return po <= m->size && a >= m->base + po && a < m->base + m->size;
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("【红】DM_SYMCHECK：%s 在 %s 哈希 %p ≠ 线性 %p", name, m->name, r, l); } }
    return r;
}
void *guest_sym(const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; }
    return NULL;
}
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const module_t *m = &g_mod[i];
        uintptr_t a = (uintptr_t)p, base = (uintptr_t)m->base, tail = (uintptr_t)m->tail_va;
        if ((a >= base && a - base < m->size) ||
            (m->patch_below && tail && a >= tail && a - tail < m->tail_len)) return &g_mod[i];
    }
    return NULL;
}

// 跨模块符号解析：先在已装模块里找定义，找不到再交给 HLE / 桩
// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return (uint64_t)p; }
    return resolve_import(m, name);
}

// 强制接管表（hle.h HLE_FORCE，__DATA,swforce 段）：命中即覆盖模块内定义
// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static void force_init(void) {
    unsigned long sz; g_force = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &sz);
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    for (unsigned i = 0; i < g_nforce; i++)
        for (unsigned j = i + 1; j < g_nforce; j++)
            if (!strcmp(g_force[i].name, g_force[j].name)) die("强制接管清单重复：%s", g_force[i].name);
}
static void *force_lookup(const char *name) {
    for (unsigned i = 0; i < g_nforce; i++)
        if (!strcmp(g_force[i].name, name)) {
            { extern void hle_hist(const char *, uint64_t, uint64_t, uint64_t); hle_hist(name, 0, 0, 0); }
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  （%s）", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // DM_HLECOUNT 时为计数桩 | A counting stub when DM_HLECOUNT is set
        }
    return NULL;
}
static void force_check(void) {
    unsigned miss = 0;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { LOG("强制接管条目无任何引用（本游戏不引用它，仅记录）：%s", g_force[i].name); miss++; }
    // 换游戏时这张表是超集：别的游戏不引用其中若干条很正常，未引用不等于配错了（原先这里 die，换游戏必崩）。
    // 全部命中时输出与旧版逐字相同，已验收的 D2R 产物行为与日志都不变。
    if (miss) LOG("force: %u 条中 %u 条未被引用（跳过，不致命）", g_nforce, miss);
    else LOG("force: %u 条全部命中", g_nforce);
}

// 读改写器表头（段边界、槽号）；表头以外有任何行即旧版产物（装载期改代码），拒绝
// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
    char line[256], key[32]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %31s %lx", key, &v) != 2) die("%s: 产物是旧改写器出的，换成当前代号（dm/PRODUCT）的产物", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else if (!strcmp(key, "PATCH_BELOW")) m->patch_below = (int)v;
        else if (!strcmp(key, "TAIL_OFF")) m->tail_off = v;
        else if (!strcmp(key, "TAIL_LEN")) m->tail_len = v;
        else if (!strcmp(key, "BSS_SIZE")) m->bss_size = v;
        else for (int k = 0; k < 8; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != 0xff)
        die("%s: 产物是旧改写器出的，换成当前代号（dm/PRODUCT）的产物", path);
    LOG("[layout] %s patch_below=%d tail_off=%#llx tail_len=%#llx bss=%#llx patch_off=%#llx key_page_off=%#llx",
        m->name, m->patch_below, (unsigned long long)m->tail_off, (unsigned long long)m->tail_len,
        (unsigned long long)m->bss_size, (unsigned long long)m->patch_off, (unsigned long long)m->key_page_off);
}

static void install_key_page(module_t *m) {
    uint8_t *kp;
    if (m->patch_below) {   // 键页在基址之下的尾部里 | the key page lives in the tail below the base
        if (m->key_page_off < m->tail_off || m->key_page_off - m->tail_off > m->tail_len ||
            m->key_page_size > m->tail_len - (m->key_page_off - m->tail_off)) die("%s: 键页越界（尾部）", m->name);
        kp = m->tail_va + (m->key_page_off - m->tail_off);
    } else {
        if (m->key_page_off + m->key_page_size > m->size) die("%s: 键页越界", m->name);
        kp = m->base + m->key_page_off;
    }
    LOG("[kp] %s patch_below=%d base=%p tail_va=%p tail_off=%#llx tail_len=%#llx key_page_off=%#llx size=%#zx kp=%p",
        m->name, m->patch_below, (void *)m->base, (void *)m->tail_va, (unsigned long long)m->tail_off,
        (unsigned long long)m->tail_len, (unsigned long long)m->key_page_off, m->size, (void *)kp);
    LOG("[kp] %s idx=%u,%u,%u,%u,%u,%u,%u,%u tsd_off=%#llx", m->name, m->key_idx[0], m->key_idx[1], m->key_idx[2],
        m->key_idx[3], m->key_idx[4], m->key_idx[5], m->key_idx[6], m->key_idx[7], (unsigned long long)g_tsd_off);
    uint64_t *page = (uint64_t *)kp;
    memset(page, 0, m->key_page_size);
#define SLOT_OFF(k) (g_tsd_off + (uint64_t)(k) * 8)
    for (unsigned i = 0; i < 4; i++) page[m->key_idx[i]] = SLOT_OFF(key_value(i));
    page[m->key_idx[KEY_KRES0]] = SLOT_OFF(g_keyres[0]); page[m->key_idx[KEY_KRES1]] = SLOT_OFF(g_keyres[1]);
    page[m->key_idx[KEY_KASPECT]] = SLOT_OFF(g_keyaspect); page[m->key_idx[KEY_KASPECTUI]] = SLOT_OFF(g_keyaspectui);
#undef SLOT_OFF
    for (int k = 0; k < 8; k++) if (!page[m->key_idx[k]]) die("%s: 键页第 %d 项为 0（宿主键没建）——游戏会读到 TSD 0 号槽", m->name, k);
    if (mprotect(kp, m->key_page_size, PROT_READ)) die("%s 键页 mprotect errno=%d (%s)", m->name, errno, strerror(errno));
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) {
                // 门返回点落在补丁区（尾部）里：巨型 bss 时尾部在基址之下 | the gate return point falls inside the patch area (tail); with a giant bss the tail is below the base
                uint64_t va = (m->patch_below && ret >= m->tail_off) ? (uint64_t)(m->tail_va + (ret - m->tail_off)) : (uint64_t)(m->base + ret);
                gate_register(va, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, m->patch_off, g_key18, g_keytls, g_keytlr, g_keygate);
}

static void relocate(module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
    }
    m->rela_off = rela; m->rela_sz = relasz; m->jmprel_off = jmprel; m->plt_sz = pltsz;   // 供 __rel_dyn_*/__rel_plt_* | for __rel_dyn_*/__rel_plt_*
    int cnt[3] = {0};
    // 诊断（DM_RELOGTYPES=1）：统计每个模块实际出现的重定位类型。缺省零影响。
    // Diagnostics (DM_RELOGTYPES=1): tally which relocation types each module actually uses. Zero effect by default.
    static const struct { int t; const char *n; } rt_names[] = {
        {285, "RELATIVE"}, {1024, "ABS64"}, {284, "JUMP_SLOT"}, {283, "GLOB_DAT"},
        {1032, "IRELATIVE"}, {257, "ABS32"}, {258, "ABS16"}, {288, "TLSDESC"}, {282, "COPY"}, {0, 0}};
    int rt_cnt[64][2]; int rt_n = 0;
    const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
    uint64_t sizes[2] = {relasz, pltsz};
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
            const Elf64_Rela *r = &tabs[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            for (int q = 0; q < rt_n; q++) if (rt_cnt[q][0] == (int)type) { rt_cnt[q][1]++; goto rt_done; }
            if (rt_n < 64) { rt_cnt[rt_n][0] = (int)type; rt_cnt[rt_n][1] = 1; rt_n++; }
        rt_done:
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si];
            uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
            if (!v) v = s->st_shndx ? (uint64_t)m->base + s->st_value : resolve_sym(m, m->strtab + s->st_name);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    LOG("%s: reloc relative=%d abs/glob=%d jump=%d", m->name, cnt[0], cnt[1], cnt[2]);
    if (getenv("DM_RELOGTYPES")) {
        for (int q = 0; q < rt_n; q++) {
            const char *nm = "(其他)";
            for (int w = 0; rt_names[w].n; w++) if (rt_names[w].t == rt_cnt[q][0]) nm = rt_names[w].n;
            LOG("  %s: reloc-type %s(%d) x%d", m->name, nm, rt_cnt[q][0], rt_cnt[q][1]);
        }
    }
}

// DM_LOADHASH=1（诊断）：装载完逐模块出地址无关 sha256，证「提前起跑 / 不提前」装载结果逐字节相同。
// 模块基址与宿主 ASLR 每局不同，只有重定位槽的值随之变：复制映像，按同一张重定位表把每个槽换成与地址无关的记号——
// RELATIVE → (0, 加数)；符号目标落在某模块 → (1 + 模块号, 模块内偏移)；否则（宿主强制接管 / 宿主符号）→ (名字 FNV, 加数)；其余字节原样。
// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
#include <CommonCrypto/CommonDigest.h>
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // 槽 8 字节换成记号头；记号尾在下面按槽序追加 | The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, cp, (CC_LONG)m->size);
        for (int t = 0; t < 2; t++)                                // 记号尾（加数 / 偏移）按槽序追加，与槽头一起唯一确定槽值 | Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                CC_SHA256_Update(&c, &tail, 8);
            }
        uint8_t d[32]; CC_SHA256_Final(d, &c); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s 大小 %#zx 重定位槽 %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

#if TARGET_OS_IPHONE
// iOS 不许运行时装入的代码执行，且苹果只许可执行文件有一个带执行权限的段（ITMS-90999）：映像由 dm/ios/tools/gen_embed.py 编进本可执行文件——
// 代码两块（原 text、补丁区）是 __TEXT 里的节，W（ro 余部 + data + bss）是单独的 RW 段。三块在文件里不相邻；这里保留一块连续地址，
// 按原相对位置 vm_remap 回去：代码页 copy=FALSE（与 __TEXT 共享同一批已签名页，r-x），W copy=TRUE（写时复制，用到才分页）。
// 运行时布局与原映像逐字节相同。iOS 上把已签名页 remap 到新地址再执行，libffi 的跳板表（FFI_EXEC_TRAMPOLINE_TABLE）就是这么做的
// iOS does not allow code loaded at run time to execute, and Apple allows an executable only one segment with execute permission (ITMS-90999): the image is compiled into this executable by dm/ios/tools/gen_embed.py --
// the two code blocks (original text, patch area) are sections in __TEXT, and W (the rest of ro + data + bss) is a separate RW segment. The three blocks are not adjacent in the file; here one contiguous address range is reserved,
// and they are vm_remap'ed back at their original relative positions: code pages with copy=FALSE (sharing the same signed pages with __TEXT, r-x), W with copy=TRUE (copy-on-write, paged only when used).
// The run-time layout is byte-identical to the original image. Remapping signed pages to a new address and then executing them on iOS is exactly what libffi's trampoline table (FFI_EXEC_TRAMPOLINE_TABLE) does
#include <mach/mach.h>
static void remap_at(module_t *m, vm_address_t to, const void *from, unsigned long size, boolean_t copy, vm_prot_t need) {
    vm_address_t a = to; vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, copy, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || a != to) die("%s vm_remap %p → %#lx（%#lx 字节）失败：%d", m->name, from, (unsigned long)to, size, kr);
    if ((cur & need) != need) die("%s vm_remap 后权限 %#x，缺 %#x", m->name, cur, need);
}
#ifdef DM_SHELL
// 壳（ios/tools/shell_build.sh 出的、不含任何游戏内容的 App）：模块映像不在主可执行文件里，在使用者自己用游戏文件做出来的
// Frameworks/Guest.framework/Guest 里——一个只装段的动态库（dm/tools/guest_dylib.py 写出），节名与内嵌做法相同，下面只换一个映像头去取。
// Shell (the App built by ios/tools/shell_build.sh, containing no game content at all): the module images are not in the main executable but in the
// Frameworks/Guest.framework/Guest that users build themselves from their game files -- a dynamic library that holds only segments (written by dm/tools/guest_dylib.py), with the same section names as the embedded approach; below only the image header to fetch from is swapped.
#include <dlfcn.h>
#include <mach-o/dyld.h>
static const struct mach_header_64 *image_mh(void) {
    static const struct mach_header_64 *mh; if (mh) return mh;
    char exe[4096]; uint32_t n = sizeof exe; if (_NSGetExecutablePath(exe, &n)) die("取不到可执行文件路径");
    char *sl = strrchr(exe, '/'); if (!sl || (size_t)(sl - exe) + 40 > sizeof exe) die("可执行文件路径不对：%s", exe); strcpy(sl, "/Frameworks/Guest.framework/Guest");
    if (!dlopen(exe, RTLD_NOW)) die("这个安装包里还没有游戏程序（%s）：先用 dm/tools/d2rm_pack.py 把你自己的游戏文件做进安装包再装。系统的说法：%s", exe, dlerror());
    for (uint32_t i = 0; i < _dyld_image_count(); i++) if (strstr(_dyld_get_image_name(i), "/Guest.framework/Guest")) mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
    if (!mh) die("Guest 动态库已加载但找不到它的映像头");
    fprintf(stderr, "boot: 模块映像取自 Guest 动态库 %s\n", exe); return mh;
}
#define IMAGE_MH image_mh()
#else
#define IMAGE_MH (&_mh_execute_header)
#endif
static void map_image(module_t *m, const char *dir) {
    (void)dir;
    char sx0[17], sx1[17], dm[17]; snprintf(sx0, sizeof sx0, "__dm%sx0", m->name); snprintf(sx1, sizeof sx1, "__dm%sx1", m->name); snprintf(dm, sizeof dm, "__DM%sW", m->name);
    unsigned long z0, zw, z1;
    const uint8_t *x0 = getsectiondata(IMAGE_MH, "__TEXT", sx0, &z0), *w = getsectiondata(IMAGE_MH, dm, "__img", &zw), *x1 = getsectiondata(IMAGE_MH, "__TEXT", sx1, &z1);
    if (!x0 || !w || !x1) die("内嵌节 %s / %s / %s 缺失", sx0, dm, sx1);
    if (z0 + zw != m->patch_off || (z0 & 0x3FFF) || (zw & 0x3FFF) || (z1 & 0x3FFF) || ((uintptr_t)x0 & 0x3FFF) || ((uintptr_t)w & 0x3FFF) || ((uintptr_t)x1 & 0x3FFF))
        die("%s 内嵌节布局不符（x0 %#lx w %#lx x1 %#lx，PATCH_OFF %#llx）", m->name, z0, zw, z1, (unsigned long long)m->patch_off);
    m->size = m->patch_off + z1;
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, m->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) die("%s 保留 %#zx 字节地址失败", m->name, m->size);
    remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
    remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    m->base = (uint8_t *)base; install_key_page(m);
}
#else
static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// 拷入匿名页后立即封代码区（页 16 KB）：[0, text_end) 与补丁区只读+执行，此后对代码区的任何写入都会当场崩；
// 之间（ro 与 data 同页只能一起）可写
// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_below) {
        size_t img = (size_t)(sz - m->tail_len);
        // 客体的 bss 在映像之后：匿名页本身是 0，只需把这段也映进来（3 GB 只占虚拟地址）
        // The guest's bss sits after the image: anonymous pages are already zero, we just have to map the range too
        m->size = (img + (size_t)m->bss_size + 0x3FFF) & ~0x3FFFull;
        uint8_t *whole = mmap(NULL, m->tail_len + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (whole == MAP_FAILED) die("%s mmap errno=%d (%s)", m->name, errno, strerror(errno));
        m->tail_va = whole; m->base = whole + m->tail_len;
        memcpy(m->base, raw, img);
        memcpy(whole, raw + img, m->tail_len);
        LOG("[map] %s mmap ok %p（尾部 %#llx 字节在基址之下，bss 不占文件）size=%#zx", m->name, m->base, (unsigned long long)m->tail_len, m->size);
        free(raw);
    } else {
        m->size = (sz + 0x3FFF) & ~0x3FFFull;
        m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m->base == MAP_FAILED) die("%s mmap errno=%d (%s)", m->name, errno, strerror(errno));
        LOG("[map] %s mmap ok %p size=%#zx", m->name, m->base, m->size);
        memcpy(m->base, raw, sz); free(raw);
    }
#ifndef __APPLE__
    // OHOS/musl：改写器发射的槽基址读取统一是 MRS xN, tpidrro_el0（0xD53BD060|N）——musl 的 TSD 在 tpidr_el0 上，
    // 装载期扫描替换为 MRS xN, tpidr_el0（0xD53BD040|N）。镜像内除发射序列外无该指令（原程序的 tpidr 读已被改写器全部改写）。
    {
        uint32_t *w = (uint32_t *)m->base;
        size_t n = (m->patch_below ? (size_t)(sz - m->tail_len) : sz) / 4;
        int patched = 0;
        for (size_t i = 0; i < n; i++)
            if ((w[i] & 0xFFFFFFE0u) == 0xD53BD060u) { w[i] = 0xD53BD040u | (w[i] & 31u); patched++; }
        if (m->patch_below) {   // 补丁区里的槽基址读取也要换 | the slot-base reads inside the patch area must be converted too
            uint32_t *t = (uint32_t *)m->tail_va; size_t tn = m->tail_len / 4;
            for (size_t i = 0; i < tn; i++) if ((t[i] & 0xFFFFFFE0u) == 0xD53BD060u) { t[i] = 0xD53BD040u | (t[i] & 31u); patched++; }
        }
        if (!patched) die("%s: 未找到槽基址读取指令（产物是 Apple 规则集？）", m->name);
        LOG("[map] %s tpidrro→tpidr 槽基址替换 %d 处", m->name, patched);
    }
#endif
    LOG("[map] %s memcpy done", m->name);
    install_key_page(m);
    LOG("[map] %s key page done", m->name);
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
    uint64_t po = m->patch_off & ~0x3FFFull;
    LOG("[map] %s mprotect 前 rx=%#llx po=%#llx text_end=%#llx patch_off=%#llx", m->name, (unsigned long long)rx, (unsigned long long)po, (unsigned long long)m->text_end, (unsigned long long)m->patch_off);
    // OHOS/部分平台 mprotect(R|X) 可能瞬态失败（真机实测：某些启动状态 EINVAL）：重试；仍失败带 errno 报红
    int ok = 0;
    for (int i = 0; i < 5 && !ok; i++) {
        int e1 = mprotect(m->base, rx, PROT_READ | PROT_EXEC);
        int e2 = e1 ? -1 : (m->patch_below
            ? mprotect(m->tail_va + (po - m->tail_off), m->tail_len - (po - m->tail_off), PROT_READ | PROT_EXEC)
            : mprotect(m->base + po, m->size - po, PROT_READ | PROT_EXEC));
        if (!e1 && !e2) { ok = 1; LOG("[map] %s mprotect ok（第 %d 次）", m->name, i + 1); break; }
        LOG("【红】%s mprotect 第 %d 次失败：errno=%d (%s)", m->name, i + 1, errno, strerror(errno));
        usleep(120000);
    }
    if (!ok) die("%s mprotect text/patch 5 次均失败（errno 见上）", m->name);
    // ★ 指令缓存同步：非 Apple 平台（OHOS/华为内核）mprotect 不自动同步 I-cache——
    //   实测匿名页写入指令后不刷缓存直接跳 → SIGILL 进程崩（真机探针复现）。
    __builtin___clear_cache((char *)m->base, (char *)m->base + m->size);
    if (m->patch_below) __builtin___clear_cache((char *)m->tail_va, (char *)m->tail_va + m->tail_len);
    LOG("[map] %s clear_cache done", m->name);
}
#endif

void load_modules(const char *dir, const char **names, int n) {
    // 先全部映射并解析符号表，再统一重定位（跨模块符号要先都在位）
    // DM_BOOTPROF：① 宿主装载细分（毫秒），定 ③ 预重定位 / mmap 的上限
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        read_layout(m, dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, dir); tm += now_ns() - t;   // 返回时代码区已是只读+执行，此后装载器只写 data | On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT：重定位完成，计数桩页封成只读可执行 | DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK：装载期共核 %lu 次符号查询，哈希与线性不一致 %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① 细分：映射（读 + 拷 + 封页）%.1f ms、解析动态段 %.1f ms、门注册 %.1f ms、重定位 + 强制接管 %.1f ms，合计 %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}

// 扮演 rtld 的 InitModules（rtld+0x830）：按装载链表正序调各模块 DT_INIT。
// 时机由 nn::init::Start 决定：nninitStartup 与 _init_libc2 之后、nnMain 之前（Start 的第 4 个参数回调）。
// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT in forward order of the load list.
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = 0; i < g_nmod; i++) {
        module_t *m = &g_mod[i];
        if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx（init_array %llu 项）", m->name, m->init, m->init_arraysz / 8);
        ((void (*)(void))(m->base + m->init))();
    }
}
