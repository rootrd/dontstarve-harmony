# P3: localising the first pixel that goes bad

Scope: the evidence chain that answers one question — **for the same resource, the same generation, the same draw, where
does a pixel first become wrong?** Everything below exists to make that answer falsifiable rather than plausible.

Related: switch reference in [DM-SWITCHES.md](DM-SWITCHES.md), prior readings in
[TEXTURE-DIAGNOSTICS.md](TEXTURE-DIAGNOSTICS.md) and [BLOCKERS.md](BLOCKERS.md).

## 1. The four stages a texel passes through

```
池内存（客体写的字节）
  └─ nvn_deswizzle_geom/nvn_deswizzle  →  解排后的线性块        A
       └─ nvn_bc_decode（CPU 软解）     →  RGBA8                B
            └─ vkCmdCopyBufferToImage  →  VkImage 里那张         rd（设备读回）
                 └─ descriptor + UV + 顶点 + 常量 + 采样 + 混合 + 写掩码  →  framebuffer → present
```

Each arrow is one rung of the ladder in §5. A round decides **one** rung for **one** subresource. If a round appears to
decide two, it decided none — the rungs are separate hypotheses and get separate artifacts.

## 2. What one round writes

All four kinds land in the directory you point at (`DM_TEXDUMP` and `DM_TEXREAD` may be the same directory).

| kind | file | bytes | row pitch recorded | gate |
|---|---|---|---|---|
| `pool` | `raw_s<serial>_v<stamp>.bin` | the whole block-linear image as the game wrote it | 0 (linear store) | layer == `DM_TEXDUMP_LAYER`, level 0 only; ≤100 images, ≤600 MB, 32 MB each |
| `A` | `tex<n>_s<serial>_v<stamp>_L<lvl>_D<la>_A.bin` | deswizzled compressed blocks | `rb` = the block row from `nvn_deswizzle_geom` | layer == `DM_TEXDUMP_LAYER`, levels 0–1 |
| `B` | `…_B.bin` | what our CPU soft-decoder made of `A` | `pad16(w × bytes-per-texel)` | same cell as `A`; absent for uncompressed images |
| `rd` | `rd_s<serial>_v<stamp>_L<lvl>_D<la>.bin` | the `VkImage` copied back off the GPU | `pad16(w × 4)` | RGBA8 images only, ≤400 images, 4 per frame, 128 MB ring |

Identity, and why each field is in the name:

* `s<serial>` — which logical texture. **Per-process counter**: it does not survive a restart and does not compare between
  rounds (§6.3).
* `v<stamp>` — the 64-bit pool-write generation this artifact was captured from (`nvn_dirty_now()` at upload time, threaded
  into all three probes). Without it, a re-upload of the same texture overwrites the previous version's evidence in place and
  the two rounds you mean to compare are the same file.
* `L<lvl>`/`D<la>` — mip level and array layer. `A`/`B`/`rd` are only comparable when **all four** fields match.
* `p3_manifest.tsv` — one line per artifact: whole-image `sha256`, exact non-zero byte count, `w/h/pitch/bytes`, and the
  `draws`/`presents` counters at the moment it was written. Each capture directory writes its own manifest (a directory
  switch is per-file, not per-process — a second directory still gets its column header).

Log lines worth keeping: `贴图落盘N …`, `整张贴N …`, `读回N …`, plus the failure lines
`贴图落盘没开` / `贴图落盘目录建不出来` / `贴图落盘写不出 …（停手）` / `读回没开` / `读回到额 …：格式不合 x 环满 y 太小 z`.
A round where the dump stopped early ("停手") is a round with a hole in it, not a round with no problem.

## 3. Recipe for one round

```
--ps DM_TEXDUMP=/data/app/el2/100/base/com.rootrd.dontstarve/haps/entry/files/tex
--ps DM_TEXREAD=/data/app/el2/100/base/com.rootrd.dontstarve/haps/entry/files/tex
--ps DM_TEXDUMP_LAYER=0
--ps DM_TEXREAD_LAYER=0
--ps DM_TEXREAD_MIN=16384
--ps DM_LOGFILE=1
--ps DM_VK_TEXTRACE=1
```

* The value of `DM_TEXDUMP`/`DM_TEXREAD` is an **absolute directory**, not `1`. Round B38 wrote `1`, the 16 log lines all
  printed, and the directory stayed empty. Both switches refuse a relative path and say so in the log.
* `DM_TEXDUMP_LAYER` and `DM_TEXREAD_LAYER` must be the **same number** in the same round, or `A`/`B` describe one
  subresource while `rd` describes another and the join correctly refuses to compare them.
* Pick the target by pixel count (`DM_TEXREAD_MIN`) or by a serial window (`DM_TEXDUMP_MIN`/`_MAX`), not by a bare serial
  you remember from last round. Serials are monotone within a process, so a ceiling stops the readback for the rest of that
  process — set it above what this round is likely to reach, or don't set it.
* Never write inside the save directory (`DM_SAVE`): round B38 is the reason the failure is reported instead of swallowed.
* Then `bash scripts/p3_pull.sh` (see below), and only then `scripts/p3_join.py`.

## 4. `scripts/p3_pull.sh` — fetch, and prove the fetch

Pulls the capture directories, `vk.log` and a screenshot, then audits **every manifest line before it is used as
evidence**: file present, byte count equal, whole-image sha256 equal. Two capture directories are merged into
`<out>/<tag>/merged/` for the join.

A truncated or overwritten artifact is caught here. If it is not caught here it will show up later as a pixel difference and
be interpreted as a rendering bug — that is how a round gets silently thrown away. The script prints
`【绿】产物自检全过` or `【红】…这轮只能报「取证缺陷」，不许报像素分界`. Install nothing, delete nothing on the device.

## 5. The ladder, and the one verdict per cell

`python3 scripts/p3_join.py <merged dir> --serial S --fmt <NVN format id> [--log run.log] [--mip N] [--layer N]`

The NVN format id comes from the log line `贴图落盘N … NVN<f>→Vk<g> 软<s> …`. Exit codes: `0` a boundary was located,
`3` evidence insufficient, `4` forensics defect (the artifacts or the manifest are wrong — fix the capture, not the game),
`2` usage error.

| rung | test | verdict | what to look at next |
|---|---|---|---|
| 1 | decode `A` offline with the reference BC decoder, compare to `B` | **CPU 已坏** | our geometry (`nvn_deswizzle_geom`: per-level offset, tileH, row pitch) or the decode call itself |
| 1′ | `A` → `B` byte-identical | CPU 侧无罪 | continue |
| 2 | `B` (padding stripped) vs `rd` | **CPU 对、GPU 坏** | the copy: `bufferRowLength`/`bufferImageHeight`, subresource selection, layout/barrier, memory visibility, the image's own extent, this texture's lifetime |
| 2′ | `B` == `rd` | **GPU 原图对、draw 后坏** | descriptor (which image/slot), UV and vertex data, push constants, sampler (lod/mip selection), blend, write mask, viewport — the join prints the bind lines and stops |
| 3 | `--frame` (internal present source) vs `--shot` (screen) | numbers only | whether the art itself is correct inside the internal source is a human call; a green mean is not a decoder verdict |
| — | any manifest/artifact mismatch, pitch that cannot hold a row, `A` without `B`, size disagreement between the two sides | **取证缺陷** | the capture, before any of the above |

Missing cells say so: no `rd` → `证据不足`, and the reason (switch off / layer mismatch / not RGBA8 / budget spent).

## 6. Three inferences this chain does not support

1. **`采样1024 非零…` is not a fill ratio.** The upload probe samples 1024 points and the pool probe reads 8 KB. They answer
   "is any content present at these addresses", which is why `p3_note` now computes the exact whole-image non-zero count and
   hashes the whole image instead. A sparse-but-correct image and an empty one can print the same probe numbers.
2. **Absence of an event is not proof it did not happen.** `[textrace]` lines are budgeted per window and are suppressed —
   the code prints `emitted=… suppressed=… (absence is inconclusive)` for exactly that reason. Same for the caps in §2: the
   first 400 images are not "all the images".
3. **Serials do not compare across rounds.** `serial` is a per-process counter; reaching a different depth in the game
   shifts every number after it. Compare `(serial, stamp, mip, layer)` tuples **inside one round**, and use pixel-count or
   presents-window gates to find the same visual thing between rounds.

Two more, added because they were nearly made this round:

4. **"A 就歪" does not prove the game's data is wrong.** It proves our deswizzle produced bytes that no longer decode — the
   suspect list is our geometry first, the source buffer second.
5. **A green pixel comparison of one mip is not a green texture.** The UI panels sample `mip>0` while full-size art samples
   `mip0`; that is why `A`/`B` are written for levels 0 and 1 in the same pass.

## 7. The offline gates that must be green first

Rung 1 leans entirely on the Python reference decoder inside `p3_join.py` being byte-identical to the bcdec that ships on the
phone. That is checked, not assumed:

| gate | command | what it proves | measured |
|---|---|---|---|
| decoder equivalence | `bash scripts/p3_bcref.sh` | one deterministic LCG block stream, decoded by production `nvn_bc_decode` and by the offline reference, compared with `cmp` | 40/40 byte-identical (BC1/BC2/BC3 × sRGB variants, 5 sizes incl. non-multiple-of-4 edges) |
| capture identity | `bash scripts/pkg_faults.sh` | artifacts carry the pool-write generation of the resource on record, a re-upload advances it, both probes agree on the layer, `DM_*_LAYER` actually moves them | 189 assertions, 0 red; the same file against `6a4def6` (`NEG=HEAD~1`, before this capture work) runs 136 and goes 51 red |
| join logic | `python3 scripts/p3_join.py --selftest` | eight fabricated rounds — CPU-broken, GPU-broken, clean, no-readback, pool-only, truncated, bad-pitch, non-RGBA8 family — each land in the right slot | all green |

The equivalence gate earned its keep: it was written first and failed immediately, because the reference decoder unpacked the
565 endpoints with R and B swapped relative to bcdec. Left unchecked it would have reported **every** correctly decoded
texture as `CPU 已坏` and burned a device round on our own tooling.

One more contract, checked on the built library before it goes anywhere near the phone: the column header that
`p3_note` writes into `p3_manifest.tsv` must be byte-identical to the `COLS` list `p3_join.py` insists on. The join
refuses a header it does not recognise (exit 4) rather than guessing column order, so a mismatch would void the round —
it is the one place where two languages have to agree without talking.

## 8. Order of work for a real round

1. Pick the target cell by pixel count / presents window (§3) and say out loud which rung you intend to test.
2. Run the round, keep `vk.log`.
3. `p3_pull.sh` → must print `【绿】产物自检全过`. If not: report 取证缺陷, stop, fix the capture.
4. `p3_join.py --serial … --fmt …` → one verdict for one cell.
5. Record the verdict and the exact numbers it printed (byte counts, first differing offset, row/channel of that offset).
   Do not start editing the renderer in the same round as the measurement — batch fixes per the standing rule.
6. Only a rung that says "CPU 侧无罪" and is confirmed by a second cell (different mip or layer) is a boundary; one cell is a
   hypothesis.
