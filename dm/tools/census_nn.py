# 清点：客体模块引用的 nn:: 服务符号里，宿主接管表还没覆盖的那些。
# 用法：python3 census_nn.py <orig 目录> <宿主仓库 dm 目录> [输出 tsv]
# Census: among the nn:: service symbols the guest modules reference, the ones the host takeover table does not cover.
# Usage: python3 census_nn.py <orig dir> <repo dm dir> [output tsv]
import struct, re, glob, sys, collections

orig, dmroot = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else "miss_nn.tsv"

def imports(p):
    img = open(p, 'rb').read()
    m = struct.unpack_from('<I', img, 4)[0]
    dyn = m + struct.unpack_from('<i', img, m + 4)[0]
    t = {}; o = dyn
    while True:
        tag, v = struct.unpack_from('<qQ', img, o); o += 16
        if tag == 0: break
        t.setdefault(tag, v)
    SYM, STR = t[6], t[5]
    n = (STR - SYM) // 24
    res = []
    for i in range(1, n):
        nm, info, oth, shn, val, sz = struct.unpack_from('<IBBHQQ', img, SYM + i * 24)
        if shn == 0:
            res.append(img[STR + nm:img.index(b'\0', STR + nm)].decode())
    return res

imp = set()
for p in sorted(glob.glob(orig + '/*.orig.bin')):
    try:
        got = imports(p)
        print('%-24s 引用 %d' % (p.split('/')[-1], len(got)))
        imp |= set(got)
    except Exception as e:
        print('跳过', p, e)

Q = chr(34)          # 双引号：源文件里用 \" 会被这里的 heredoc 吞掉 | double quote, dodging shell escaping
have = set()
for f in glob.glob(dmroot + '/**/*.c', recursive=True) + glob.glob(dmroot + '/**/*.h', recursive=True):
    s = open(f, encoding='utf-8', errors='ignore').read()
    for pat in (r'HLE_FORCE\(' + Q + r'(.*?)' + Q, r'HLE\(' + Q + r'(.*?)' + Q):
        have |= set(re.findall(pat, s))

nn = [x for x in imp if x.startswith('_ZN2nn')]
miss = sorted(x for x in nn if x not in have)
print('客体引用 %d，宿主表 %d，nn:: 引用 %d，未覆盖 %d' % (len(imp), len(have), len(nn), len(miss)))
c = collections.Counter()
for x in miss:
    m = re.match(r'_ZN2nn(\d+)([a-z]+)', x)
    c[m.group(2) if m else '?'] += 1
for k, v in c.most_common(40):
    print('  %4d  nn::%s' % (v, k))
open(out, 'w').write('\n'.join(miss) + '\n')
print('明细 →', out)
