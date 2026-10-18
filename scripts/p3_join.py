#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P3 取证离线联判 | the offline join for one capture round.

一轮采集回来的产物（scripts/p3_pull.sh 收的目录）在这里对齐。脚本只读 p3_manifest.tsv，不猜文件名：

    清单自检 → 同一 (serial, 版) 内的身份配对 → CPU 侧参考解码 → 与 GPU 读回逐字节 → 按 P3 的梯子给**一个**分界

分界口径（指令 P3 那四条，每次只验一个）：
  CPU 已坏            A 按 BC 定义解不出 B 那份像素 ⇒ 查实际 NVN 几何、BC 格式/解码
  CPU 对、GPU 坏      B 与参考解码一致，但读回不符   ⇒ 查 copy 范围/row pitch、可见性、barrier、子资源、生命周期
  GPU 原图对、draw 后坏  B == 读回                  ⇒ 查 descriptor、UV/顶点、常量、采样、混合、写掩码（脚本只到这儿，之后要人眼）
  present 源对、屏幕坏   只出差异数字，不出结论      ⇒ 「内部源里这块美术本身对不对」必须人眼看

用法 | usage:
    python3 p3_join.py <采集目录> [--serial S] [--mip N] [--layer N] [--fmt 0x44] [--log run.log]
                       [--frame 整帧.raw] [--shot 屏幕.jpg] --w 2848 --h 1276 [--verbose]
    python3 p3_join.py --selftest [临时目录]

退出码：0 给出分界｜3 证据不足，不许下结论｜4 取证缺陷（产物或清单本身错了）｜2 用法错。
"""
import argparse
import hashlib
import os
import re
import struct
import sys

COLS = ["kind", "serial", "stamp", "mip", "layer", "w", "h", "pitch", "bytes",
        "sha256", "nonzero", "draws", "presents", "path"]
NUM = ("serial", "stamp", "mip", "layer", "w", "h", "pitch", "bytes", "nonzero", "draws", "presents")

# NVN 压缩格式号 → 每块字节；参考解码器只覆盖 BC1/2/3（DXT1/3/5），这三族是饥荒世界美术在用的
BC_BYTES = {0x41: 8, 0x42: 8, 0x45: 8, 0x46: 8, 0x43: 16, 0x47: 16, 0x44: 16, 0x48: 16}
HAS_ALPHA = (0x43, 0x47, 0x44, 0x48)
# 软解后每像素字节：照 dm/gfx/tex/nvn_bcdec.c 的 nvn_bc_unpacked（BC1/2/3/7 出 RGBA8，BC4/5 出 1/2 字节，BC6H 出半精度 RGBA）。
# 清单里 B 那半的行距就是按这个算的；比对再写死 4，等于自己造差异。
UNPACK_BPP = {0x49: 1, 0x4a: 1, 0x4b: 2, 0x4c: 2, 0x4f: 8, 0x50: 8}


def pad16(w, bpp):
    return ((w * bpp + 15) // 16) * 16


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def die(code, msg):
    print("【红】" + msg)
    sys.exit(code)


# ---- 清单 ----
def read_manifest(d):
    mp = os.path.join(d, "p3_manifest.tsv")
    if not os.path.isfile(mp):
        die(4, "取证缺陷：%s 里没有 p3_manifest.tsv —— 这一轮的产物没有身份表，任何联判都是在猜" % d)
    rows = []
    with open(mp, "r", encoding="utf-8", newline="") as f:
        head = f.readline().rstrip("\n").split("\t")
        if head != COLS:
            die(4, "取证缺陷：清单列头与生产不一致\n  期望 %s\n  实际 %s" % ("|".join(COLS), "|".join(head)))
        for ln, line in enumerate(f, 2):
            c = line.rstrip("\n").split("\t")
            if len(c) != len(COLS):
                die(4, "取证缺陷：清单第 %d 行字段数 %d（应 %d）：%s" % (ln, len(c), len(COLS), line.strip()))
            r = dict(zip(COLS, c))
            try:
                for k in NUM:
                    r[k] = int(r[k])
            except ValueError:
                die(4, "取证缺陷：清单第 %d 行有字段不是数字：%s" % (ln, line.strip()))
            rows.append(r)
    if not rows:
        die(4, "取证缺陷：清单是空的——开关没开、目录不可写，或额度花在别的批次上了")
    return rows


def audit(d, rows, verbose):
    """先证「取证本身没错」：产物在位、字节数与清单一致、整幅哈希重算一致。任何一条不符就整轮作废。"""
    bad = []
    for r in rows:
        p = os.path.join(d, r["path"])
        if not os.path.isfile(p):
            bad.append("缺文件 %s" % r["path"]); continue
        sz = os.path.getsize(p)
        if sz != r["bytes"]:
            bad.append("尺寸不符 %s：清单 %d 实际 %d" % (r["path"], r["bytes"], sz)); continue
        got = sha256_file(p)
        if got != r["sha256"]:
            bad.append("哈希不符 %s：清单 %s… 实际 %s…（取回途中截断，或同名被覆盖过）" % (r["path"], r["sha256"][:12], got[:12]))
            continue
        if verbose:
            print("  自检过 %s 字节%d 非零%d" % (r["path"], sz, r["nonzero"]))
    return bad


def group(rows, serial, mip, layer):
    g = {}
    for r in rows:
        if serial is not None and r["serial"] != serial:
            continue
        if mip is not None and r["mip"] != mip:
            continue
        if layer is not None and r["layer"] != layer:
            continue
        g.setdefault((r["serial"], r["stamp"]), []).append(r)
    return g


def cell(rows, kind, serial, stamp, mip, layer):
    for r in rows:
        if (r["kind"], r["serial"], r["stamp"], r["mip"], r["layer"]) == (kind, serial, stamp, mip, layer):
            return r
    return None


def load(d, r):
    return open(os.path.join(d, r["path"]), "rb").read()


def strip_pad(buf, w, h, pitch, bpp):
    """剔行 padding：只取每行前 w*bpp 字节。两边都是我们自己的落盘口径（左上原点、行序相同），不改向。"""
    need = w * bpp
    if pitch < need or len(buf) < pitch * h:
        return None
    out = bytearray()
    for y in range(h):
        s = y * pitch
        out += buf[s:s + need]
    return bytes(out)


# ---- BC 参考解码：照 dm/third_party/bcdec/bcdec.h 的整数算术逐位复刻（bcdec 不做 sRGB 转换，所以比对在编码域逐字节）----
def _c565(v):
    # bcdec 的取位顺序：r 在高 5 位、g 占中间 6 位、b 在低 5 位。这里手抄反过一次，
    # 对拍当场判红——R/B 掉了个个儿，「CPU 已坏」那一档就会把好产物读成坏像素。
    return (v >> 11, (v >> 5) & 0x3F, v & 0x1F)


def _e5(v):
    return (v * 527 + 23) >> 6


def _e6(v):
    return (v * 259 + 33) >> 6


def color_block(blk, opaque_only):
    c0, c1 = struct.unpack_from("<HH", blk, 0)
    r0, g0, b0 = _c565(c0); r1, g1, b1 = _c565(c1)
    pal = [(_e5(r0), _e6(g0), _e5(b0), 255), (_e5(r1), _e6(g1), _e5(b1), 255)]
    if c0 > c1 or opaque_only:
        pal.append((((2 * r0 + r1) * 351 + 61) >> 7, ((2 * g0 + g1) * 2763 + 1039) >> 11, ((2 * b0 + b1) * 351 + 61) >> 7, 255))
        pal.append((((r0 + 2 * r1) * 351 + 61) >> 7, ((g0 + 2 * g1) * 2763 + 1039) >> 11, ((b0 + 2 * b1) * 351 + 61) >> 7, 255))
    else:                                            # 少见的 DXT1 分档：第四色是全透明黑
        pal.append((((r0 + r1) * 1053 + 125) >> 8, ((g0 + g1) * 4145 + 1019) >> 11, ((b0 + b1) * 1053 + 125) >> 8, 255))
        pal.append((0, 0, 0, 0))
    idx = struct.unpack_from("<I", blk, 4)[0]
    px = []
    for _y in range(4):
        for _x in range(4):
            px.append(pal[idx & 3]); idx >>= 2
    return px


def bc2_alpha(blk):
    a = []
    for y in range(4):
        v = struct.unpack_from("<H", blk, y * 2)[0]
        for x in range(4):
            a.append(((v >> (4 * x)) & 0xF) * 17)
    return a


def bc3_alpha(blk):
    a0, a1 = blk[0], blk[1]
    if a0 > a1:
        t = [a0, a1, (6 * a0 + a1) // 7, (5 * a0 + 2 * a1) // 7, (4 * a0 + 3 * a1) // 7,
             (3 * a0 + 4 * a1) // 7, (2 * a0 + 5 * a1) // 7, (a0 + 6 * a1) // 7]
    else:
        t = [a0, a1, (4 * a0 + a1) // 5, (3 * a0 + 2 * a1) // 5, (2 * a0 + 3 * a1) // 5, (a0 + 4 * a1) // 5, 0, 255]
    bits = struct.unpack_from("<Q", blk, 0)[0] >> 16
    out = []
    for _y in range(4):
        for _x in range(4):
            out.append(t[bits & 7]); bits >>= 3
    return out


def ref_decode(src, pitch, w, h, nvn_fmt):
    """解排后的压缩块 → RGBA8（行距 pad16(w,4)）；边上不满 4x4 的块只拷有效像素，与 nvn_bc_decode 同法。"""
    bb = BC_BYTES[nvn_fmt]
    if pitch % bb:
        return None
    bpr = pitch // bb
    dp = pad16(w, 4)
    dst = bytearray(dp * h)
    for by in range((h + 3) // 4):
        for bx in range(bpr):
            off = by * pitch + bx * bb
            if off + bb > len(src):
                return None
            blk = src[off:off + bb]
            if nvn_fmt in HAS_ALPHA:
                px = color_block(blk[8:], True)
                al = bc2_alpha(blk) if nvn_fmt in (0x43, 0x47) else bc3_alpha(blk)
                px = [(r, g, b, al[i]) for i, (r, g, b, _a) in enumerate(px)]
            else:
                px = color_block(blk, False)
            cw = min(4, w - bx * 4); ch = min(4, h - by * 4)
            for y in range(ch):
                s = (by * 4 + y) * dp + bx * 4 * 4
                for x in range(cw):
                    dst[s + x * 4:s + x * 4 + 4] = bytes(px[y * 4 + x])
    return bytes(dst)


BIND_RE = re.compile(r"\[textrace\] frame=(\d+) event=(\S+) tid=(-?\d+) serial=(\d+) image=([0-9a-f]+) "
                     r"u=(\d+) d=(\d+) bad=(\d+) tried=(\d+) (.*)")


def bind_lines(log, serial, n=2):
    out = []
    if not log:
        return out, 0
    with open(log, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = BIND_RE.search(line)
            if m and int(m.group(4)) == serial:
                out.append(line.rstrip())
    return out[:n], len(out)


def verdict_for(d, rows, key, args):
    """一个 (serial, 版) 的判定。只给一个分界；证据不齐就明说缺哪一格，绝不外推。"""
    serial, stamp = key
    mine = [r for r in rows if r["serial"] == serial and r["stamp"] == stamp]
    ab = sorted({(r["mip"], r["layer"]) for r in mine if r["kind"] in ("A", "B")})
    rdc = sorted({(r["mip"], r["layer"]) for r in mine if r["kind"] == "rd"})
    pool = cell(rows, "pool", serial, stamp, 0, 0)
    print("\n=== serial=%d 版=%d｜CPU 格(mip,layer)=%s｜读回格=%s" % (serial, stamp, ab or "无", rdc or "无"))
    if pool:
        print("    池里整张：%s 字节%d 非零%d（第 %d 画时打的）" % (pool["path"], pool["bytes"], pool["nonzero"], pool["draws"]))
    else:
        print("    池里整张：无（raw 额度到顶或这张没进窗口）——不影响下面比对，但离线穷举每级几何就没料了")

    if not ab:
        return "证据不足", "这一世只有读回没有 CPU 侧产物，两边没得比：把 DM_TEXDUMP_LAYER 与 DM_TEXREAD_LAYER 指到同一格再跑"

    def cpu_of(c):
        """一格里的「CPU 侧线性像素」：软解图取 B（解完块后的 RGBA），未压缩图取 A（解排后的线性像素）。"""
        b = cell(rows, "B", serial, stamp, c[0], c[1])
        if b is not None:
            return b, b
        a = cell(rows, "A", serial, stamp, c[0], c[1])
        return a, None

    # 挑一格开判：优先挑 CPU 侧与读回都在位的那格
    tgt = next((c for c in ab if c in rdc), ab[0])
    cpu, b_row = cpu_of(tgt)
    if cpu is None:
        return "证据不足", "第 %d 级第 %d 层既没有 B 也没有 A：落盘那趟没记上这一格" % tgt
    if b_row is None:
        print("    未压缩（只有 A）：按 4 字节/像素直接拿 A 当 CPU 线性像素与读回比；其它 bpp 的图要用 --fmt 说明后另判")
    # 读回那半边设备侧只采 RGBA8 的图（tex_read_arm 里 v->fmt 那条判据）。软解出来不是 4 字节/像素的这族，
    # 两边根本不是同一个域，硬比只会产出一条我们自己编的「GPU 坏」。
    bpp = UNPACK_BPP.get(args.fmt, 4) if b_row is not None and args.fmt is not None else 4
    if bpp != 4:
        return "证据不足", "这格式软解后是 %d 字节/像素（清单里 B 的行距也是按它算的），而读回只采 RGBA8：第二档在这族上比不了，换一张 BC1/2/3 的图或先扩读回" % bpp

    # ① CPU 自洽：A 按定义解码应等于 B（只有压缩族有这一步）
    if b_row is not None:
        if args.fmt is None:
            print("    CPU 参考解码：没给 --fmt（NVN 格式号），这一档跳过。号在日志「贴图落盘N … NVNa→Vkb」里")
        elif args.fmt not in BC_BYTES:
            print("    CPU 参考解码：NVN 0x%x 本脚本没有离线解码器（BC4/5/6H/7 未实现）" % args.fmt)
        else:
            a_row = cell(rows, "A", serial, stamp, tgt[0], tgt[1])
            if a_row is None:
                return "证据不足", "有 B 没 A：压缩图只落了一半，落盘本身要查"
            ref = ref_decode(load(d, a_row), a_row["pitch"], a_row["w"], a_row["h"], args.fmt)
            if ref is None:
                return "取证缺陷", "A 的行距 %d 与块字节 %d 除不尽：这一格的几何自相矛盾，先查量尺" % (a_row["pitch"], BC_BYTES[args.fmt])
            got = load(d, b_row)
            if len(got) != len(ref):
                return "取证缺陷", "B 的字节数 %d 与按 w/h/pitch 算出的 %d 不等" % (len(got), len(ref))
            diff = sum(1 for i in range(len(ref)) if ref[i] != got[i])
            if diff:
                first = next(i for i in range(len(ref)) if ref[i] != got[i])
                mx = max(abs(ref[i] - got[i]) for i in range(len(ref)))
                print("    CPU 参考解码：%d/%d 字节不同（最大差 %d；首个在第 %d 字节 = 第 %d 行 第 %d 通道）" %
                      (diff, len(ref), mx, first, first // b_row["pitch"], (first % b_row["pitch"]) % 4))
                return "CPU 已坏", "解排后的压缩块按 BC 定义解不出 B 那份像素：查实际 NVN 几何（每级起点/tileH/行距）与解码调用"
            print("    CPU 参考解码：A 解出 %d 字节与 B 逐字节一致 ⇒ CPU 侧无罪" % len(ref))

    # ② GPU 里那张 vs CPU 那份
    r_row = cell(rows, "rd", serial, stamp, tgt[0], tgt[1])
    if r_row is None:
        return "证据不足", "第 %d 级第 %d 层没有读回（DM_TEXREAD 没开、层号没对上、或那张的格式不是 RGBA8）：%s" % \
               (tgt[0], tgt[1], "分界还差 GPU 这一半")
    if (r_row["w"], r_row["h"]) != (cpu["w"], cpu["h"]):
        return "取证缺陷", "读回尺寸 %dx%d 与 CPU 那份 %dx%d 不等：两边采的不是同一格" % \
               (r_row["w"], r_row["h"], cpu["w"], cpu["h"])
    gpu = strip_pad(load(d, r_row), r_row["w"], r_row["h"], r_row["pitch"], 4)
    lin = strip_pad(load(d, cpu), cpu["w"], cpu["h"], cpu["pitch"], 4)
    if gpu is None or lin is None:
        return "取证缺陷", "行距放不下像素（读回行%d / CPU 行%d，需 %d）" % (r_row["pitch"], cpu["pitch"], pad16(r_row["w"], 4))
    if gpu == lin:
        print("    GPU 读回：%d 字节与 CPU 那份逐字节一致 ⇒ 上传链（copy 范围/行距/屏障/布局）无罪" % len(lin))
        heads, n = bind_lines(args.log, serial)
        print("    下一个分界在 draw：descriptor/UV/顶点/常量/采样/混合/写掩码（脚本不判，要人眼）")
        if n:
            print("    该 serial 的 bind 证据 %d 条，头 %d 条：" % (n, len(heads)))
            for x in heads:
                print("      " + x)
        elif args.log:
            print("    日志里没有这个 serial 的 bind 行（窗口或额度没排到），别据此说它没被绑过")
        else:
            print("    没带 --log：绑定那一层的证据拿不出来，下一轮记得收样日志")
        return "GPU 原图对、draw 后坏", "CPU 与 GPU 一致：分界在 draw，不在拷贝"
    diff = sum(1 for i in range(len(lin)) if lin[i] != gpu[i])
    print("    GPU 读回：%d/%d 字节与 CPU 不同 ⇒ 字节是搬进 GPU 之后才变坏的" % (diff, len(lin)))
    return "CPU 对、GPU 坏", "查 copy 范围/row pitch、内存可见性、barrier、子资源选择与这张的生命周期"


def frame_shot_cmp(args):
    """第 4 档只出数字：内部 present 源与屏幕截图的差异。结论要人确认「内部源里这块美术本身对不对」。"""
    if not (args.frame and args.shot):
        return
    if not args.w or not args.h:
        print("\n[present 面] 要判这一档得同时给 --frame --shot --w --h"); return
    raw = open(args.frame, "rb").read()
    need = args.w * args.h * 4
    if len(raw) < need:
        print("\n[present 面] 帧数据 %d 字节不够 %dx%d x4：口径没对上，这一档不比" % (len(raw), args.w, args.h)); return
    try:
        from PIL import Image
    except Exception:
        print("\n[present 面] 本机没有 PIL，读不进屏幕图：这一档跳过（不影响前三档）"); return
    sp = Image.open(args.shot).convert("RGBA").resize((args.w, args.h)).tobytes()
    tot = sum(abs(raw[i + k] - sp[i + k]) for i in range(0, need, 4) for k in range(3))
    n = args.w * args.h * 3
    bad = 0
    for y in range(args.h):
        for x in range(0, args.w, 4):
            j = (y * args.w + x) * 4
            bad += sum(abs(raw[j + k] - sp[j + k]) for k in range(3)) > 24
    print("\n[present 面] 内部源 vs 屏幕：三通道平均差 %.2f/255，抽样(每行 1/4)超阈 %d/%d 像素" % (tot / n, bad, args.w * args.h // 4))
    print("            差异小 ⇒ 屏与内部源一致，绿地得往上游找；差异大 ⇒ 才轮到 present 链。"
          "「绿色均值」不是解码坏的判据，这一档不给分界结论。")


# ---- 自测：造四种情形，断言判定各归各位（离线跑，不接设备）----
def selftest(tmp):
    root = tmp or os.path.join(os.environ.get("TMPDIR", "/tmp"), "p3_selftest")
    ok = True
    W, H, SER, STAMP = 8, 8, 1234, 77
    dp = pad16(W, 4)

    def make_blocks():
        """造一份真实排布的 BC3 块流：行距 = 块列数 x 16，每块彩色段用两个端点、alpha 段用插值表。"""
        bpr = (W + 3) // 4
        rows = (H + 3) // 4
        out = bytearray(bpr * 16 * rows)
        for by in range(rows):
            for bx in range(bpr):
                o = by * bpr * 16 + bx * 16
                out[o] = 250; out[o + 1] = 5                      # alpha 两端
                struct.pack_into("<Q", out, o, struct.unpack_from("<Q", out, o)[0] | (0x123456 << 16))
                struct.pack_into("<HHI", out, o + 8, 0xF800, 0x001F, 0xE4E4E4E4)
        return bytes(out), bpr * 16

    def row(kind, mip, layer, w, h, pitch, data, path, draws=7, presents=3):
        nz = sum(1 for b in data if b)
        return "\t".join([kind, str(SER), str(STAMP), str(mip), str(layer), str(w), str(h), str(pitch),
                          str(len(data)), hashlib.sha256(data).hexdigest(), str(nz), str(draws), str(presents), path])

    def build(name, lines, files):
        d = os.path.join(root, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "p3_manifest.tsv"), "w", encoding="utf-8", newline="") as f:
            f.write("\t".join(COLS) + "\n")
            for x in lines:
                f.write(x + "\n")
        for nm, data in files.items():
            with open(os.path.join(d, nm), "wb") as f:
                f.write(data)
        return d

    a_bytes, a_pitch = make_blocks()
    good = ref_decode(a_bytes, a_pitch, W, H, 0x44)
    assert good and len(good) == dp * H, "参考解码自身的尺寸就是错的，这把尺子不能用了"
    # 料不能是退化的：四种索引档都得出现，否则「CPU 已坏」这一档是在拿一块纯色比纯色
    pal = {good[i:i + 4] for i in range(0, len(good), 4)}
    assert len(pal) >= 3, "自测的块流只解出 %d 种颜色，判据咬不到东西" % len(pal)
    assert {g[3] for g in pal} != {255}, "自测的块流没走到 alpha 插值表：BC3 的 alpha 半边没被验过"

    b_bad = bytearray(good); b_bad[dp + 4] ^= 0x40                 # CPU 那份像素被写坏一格
    r_bad = bytearray(good); r_bad[2 * dp] = (r_bad[2 * dp] + 1) & 0xFF   # GPU 里那格与 CPU 不同

    cases = [
        ("cpu_bad", "CPU 已坏", a_bytes, bytes(b_bad), good),
        ("gpu_bad", "CPU 对、GPU 坏", a_bytes, good, bytes(r_bad)),
        ("clean", "GPU 原图对、draw 后坏", a_bytes, good, good),
        ("no_rd", "证据不足", a_bytes, good, None),
        ("only_pool", "证据不足", None, None, None),
    ]
    for name, want, ab_, bb_, rb_ in cases:
        lines, files = [], {}
        if ab_ is not None:
            lines.append(row("pool", 0, 0, W, H, 0, ab_[:64], "raw.bin"))
            lines.append(row("A", 0, 0, W, H, a_pitch, ab_, "a.bin"))
            lines.append(row("B", 0, 0, W, H, dp, bb_, "b.bin"))
            files.update({"raw.bin": ab_[:64], "a.bin": ab_, "b.bin": bb_})
        else:
            lines.append(row("pool", 0, 0, W, H, 0, b"\x11" * 64, "raw.bin"))
            files["raw.bin"] = b"\x11" * 64
        if rb_ is not None:
            lines.append(row("rd", 0, 0, W, H, dp, rb_, "rd.bin"))
            files["rd.bin"] = rb_
        d = build(name, lines, files)
        ns = argparse.Namespace(fmt=0x44, log=None, verbose=False)
        rows = read_manifest(d)
        bad = audit(d, rows, False)
        got, why = (verdict_for(d, rows, (SER, STAMP), ns) if not bad else ("取证缺陷", "清单与产物对不上"))
        flag = "【绿】" if got == want else "【红】"
        ok &= (got == want)
        print("%s %-9s 判得 %-22s 期望 %-22s（%s）" % (flag, name, got, want, why))

    # 取证缺陷必须当场抓住：截断的产物不能在联判里冒充证据
    d = build("hashbad", [row("B", 0, 0, W, H, dp, good, "b.bin")], {"b.bin": good[:-1]})
    rows = read_manifest(d)
    bad = audit(d, rows, False)
    flag = "【绿】" if bad else "【红】"
    ok &= bool(bad)
    print("%s %-9s 抓到 %d 条：%s" % (flag, "hashbad", len(bad), bad[0] if bad else "没抓到截断"))

    # 行距不够放一行像素：这是取证缺陷，不是「像素坏了」
    lines = [row("A", 0, 0, W, H, a_pitch, a_bytes, "a.bin"),
             row("B", 0, 0, W, H, dp, good, "b.bin"),
             row("rd", 0, 0, W, H, W * 2, good[:W * 2 * H], "rd.bin")]
    d = build("pitchbad", lines, {"a.bin": a_bytes, "b.bin": good, "rd.bin": good[:W * 2 * H]})
    rows = read_manifest(d)
    ns = argparse.Namespace(fmt=0x44, log=None, verbose=False)
    got, why = verdict_for(d, rows, (SER, STAMP), ns)
    flag = "【绿】" if got == "取证缺陷" else "【红】"
    ok &= (got == "取证缺陷")
    print("%s %-9s 判得 %-22s 期望 取证缺陷（%s）" % (flag, "pitchbad", got, why))

    # 非 4 字节/像素那族（BC4/5/6H）：设备侧读回压根不采这种图，第二档不许硬比出个「GPU 坏」。
    d = build("bc4", [row("A", 0, 0, W, H, a_pitch, a_bytes, "a.bin"),
                      row("B", 0, 0, W, H, dp, good, "b.bin")], {"a.bin": a_bytes, "b.bin": good})
    rows = read_manifest(d)
    got, why = verdict_for(d, rows, (SER, STAMP), argparse.Namespace(fmt=0x49, log=None, verbose=False))
    flag = "【绿】" if got == "证据不足" else "【红】"
    ok &= (got == "证据不足")
    print("%s %-9s 判得 %-22s 期望 证据不足（%s）" % (flag, "bc4", got, why))

    print("\n== 自测 %s" % ("全绿" if ok else "有红"))
    return 0 if ok else 1


def refdump(a):
    """离线半边：把 bcref_equiv 生成的块流按参考解码器解一遍，落一份 RGBA 好与生产那份 cmp。
    行距公式与 nvn_bc_decode 逐字对齐：块行 = ceil(w/4) x 每块字节，像素行 = pad16(w,4)。"""
    fmt, w, h, src, dst = a.refdump
    fmt = int(fmt, 0)
    if fmt not in BC_BYTES:
        die(2, "参考解码器只做 BC1/2/3（0x41–0x48），0x%x 不在内" % fmt)
    w, h = int(w), int(h)
    bb = BC_BYTES[fmt]
    pitch = ((w + 3) // 4) * bb
    blob = open(src, "rb").read()
    out = ref_decode(blob, pitch, w, h, fmt)
    if out is None:
        die(2, "块流 %s 不够 %dx%d 的 %d 字节：尺寸或格式号给错了" % (src, w, h, pitch * ((h + 3) // 4)))
    open(dst, "wb").write(out)
    print("ok 参考解码 0x%x %dx%d 块行%d 像素行%d → %s（%d 字节）" % (fmt, w, h, pitch, pad16(w, 4), dst, len(out)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?")
    ap.add_argument("--serial", type=int)
    ap.add_argument("--mip", type=int)
    ap.add_argument("--layer", type=int)
    ap.add_argument("--fmt", type=lambda s: int(s, 0), help="NVN 压缩格式号（0x44=BC3/DXT5）：CPU 档的离线参考解码要用")
    ap.add_argument("--log", help="收样那趟的日志（取 bind 证据行）")
    ap.add_argument("--frame", help="内部 present 源的整帧（RGBA）")
    ap.add_argument("--shot", help="同屏的屏幕截图")
    ap.add_argument("--w", type=int); ap.add_argument("--h", type=int)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--refdump", nargs=5, metavar=("FMT", "W", "H", "BLOCKS", "OUT"),
                    help="与生产解码器对拍的离线半边：把块流解成 RGBA")
    a = ap.parse_args()

    if a.refdump:
        sys.exit(refdump(a))
    if a.selftest:
        sys.exit(selftest(a.dir))
    if not a.dir:
        die(2, "用法：p3_join.py <采集目录> [--serial S --fmt 0x44] [--log run.log]，或 --selftest")

    rows = read_manifest(a.dir)
    bad = audit(a.dir, rows, a.verbose)
    if bad:
        print("【红】取证缺陷：产物与清单对不上，这一轮不许出像素结论（%d 条）" % len(bad))
        for x in bad[:20]:
            print("   - " + x)
        sys.exit(4)
    dist = {k: sum(1 for r in rows if r["kind"] == k) for k in ("pool", "A", "B", "rd")}
    print("== 清单自检全过：%d 行产物｜kind 分布 %s" % (len(rows), dist))

    g = group(rows, a.serial, a.mip, a.layer)
    if not g:
        die(3, "证据不足：清单里没有满足 --serial/--mip/--layer=%s 的产物" % ((a.serial, a.mip, a.layer),))
    tally = {}
    for key in sorted(g):
        v, why = verdict_for(a.dir, rows, key, a)
        print("    分界：%s —— %s" % (v, why))
        tally.setdefault(v, []).append(key)
    frame_shot_cmp(a)
    print("\n== 本轮分界汇总（每格只归一个档）")
    for v, keys in tally.items():
        print("   %-22s %d 格：%s" % (v, len(keys), ", ".join("%d/v%d" % k for k in keys[:8])))
    sys.exit(3 if set(tally) == {"证据不足"} else 0)


if __name__ == "__main__":
    main()
