# Launch switches

Every `--ps` parameter passed to `aa start` whose name begins with `DM_` is pulled out of the launch JSON and turned into an environment variable inside the host, before the guest is loaded. So the whole diagnostic surface is reachable without rebuilding anything. (There are ~400 switches in the tree; most belong to the Diablo II branch. Listed here are the ones this port uses.)

```bash
hdc shell aa start -a EntryAbility -b <bundleName> \
  --ps DM_MODULES main,sdk,subsdk0 --ps DM_FS 1 --ps DM_TITLE_DRV 0 \
  --ps DM_SAMPLE 2000 --ps DM_SPUR 500 --ps DM_CRASHDIR <files dir>
```

## What the guest is

| Switch | Values | Meaning |
|---|---|---|
| `DM_MODULES` | `main,sdk,subsdk0` | which modules to load, in load order |
| `DM_IMPORTS` | `0`/`1` | print every imported symbol the host has to satisfy — the first thing to turn on when a new title aborts at startup |
| `DM_TITLE_DRV` | `0` for this game | enable the per-title gameplay drivers written for Diablo II. **Must be 0**: they index into the guest object by a D2R offset and will crash any other title |
| `DM_APP_POOL_ADD_MB` | MB, default 512 | how much more memory the host reports to the guest than the Switch's 3,285 MB. Raise it when the guest aborts on its own allocation limit |

## File system and data

| Switch | Values | Meaning |
|---|---|---|
| `DM_FS` | `1` | route the guest's file I/O through the host mapper |
| `DM_MISSLIST` | file path | append every guest path that could not be resolved. **The healthy result is that the file is never created.** This is the single most useful switch when a screen looks wrong |
| `DM_FSLOG`, `DM_FSLOG_HASH` | path / `1` | full open() trace, optionally hashed |
| `DM_SAVE` | path | the guest's writable save directory. Note the game cleans its own `save/` — never put diagnostic output inside it |

## Diagnostics that survive a crash

| Switch | Values | Meaning |
|---|---|---|
| `DM_CRASHDIR` | directory | crash scenes are written to `crash_<pid>.txt` here (signal, fault address, pc/lr/sp, module-relative offsets, thread name, SVC count). The sandbox path readable over `hdc` is `/data/app/el2/100/base/<bundleName>/haps/entry/files/` |
| `DM_CRASH_OFF` | `1` | disable the handler (only to prove something else installed it) |
| `DM_SAMPLE` | **milliseconds**, optionally `ms:threadname` or `ms:*` | **OHOS default: `0` (disabled).** Periodic sampler line, incl. the last SVC number and its `x0` (`svc=0x1c a0=0x…`) per thread. The unit is **ms**, not µs — `DM_SAMPLE 100000` is one line every 100 s. `0` (or anything negative) now means **off**: before that guard it started a 0 ms-period thread, which measured 2 GB of log in 8 minutes and visibly heated the device. Anything under ~2000 evicts your own diagnostics from the log. **Use 2000** for a normal round. `:*` cycles every live guest thread instead of only the main one — the only way to see a worker that stalled without the main thread noticing |
| `DM_HBEAT` | seconds | heartbeat block inside the sampler: NVN call census, HLE entry/SVC counts, then one line per non-empty per-thread call ring (handle, name, total, **new since the last beat**, last three calls) plus the full rings that moved. A sample line says where a thread sits; the beat says whether it called anything in between — counts frozen between two beats = really stopped, counts climbing with no picture = looping, and the entries that climb are what it keeps waiting on. **Use 10.** Needs `DM_SAMPLE` on (the sampler prints it) |
| `DM_HLTRACE` | `1` | turn every host-side counting stub into a counting **+ argument-recording** stub, feeding those per-thread rings (`x0..x2`, caller pc). Turn on when a thread stops after a call and you need to know with what arguments. Costs a second executable mapping, so use it on a diagnostic round only |
| `DM_THRDUMP` | ms, or `0` | dump all guest threads (what each waits on, the holder, the condition key value, last SVC + lr) into `thr.txt` |
| `DM_BOOTPROF` | ms | startup phase timing |
| `DM_DBG` | mask | general debug bits; `DM_DBG_COVER` for coverage |
| `DM_HLECOUNT`, `DM_HLECOUNT_LOG` | `1` | count host-side service calls by symbol — how you find out which `nn::*` entry is being hammered |

## Threading

| Switch | Values | Meaning |
|---|---|---|
| `DM_SPUR` | ms (we use 500) | spurious-wake fallback for condition waits. **Required today**: without it the shard's `Server Master` is never woken and world generation never finishes. This masks an unfixed root cause — see [BLOCKERS.md](BLOCKERS.md#2-lost-wakeup-worked-around-root-cause-unknown) |

## Graphics

| Switch | Values | Meaning |
|---|---|---|
| `DM_RAW` | frame count | dump that many whole presented frames as `p<serial>_f<present>_<w>x<h>.rgba` (RGBA8). Convert with `scripts/rgbapng.ps1` |
| `DM_RAWDIR` | directory | where those frames go. Must be outside `DM_SAVE` |
| `DM_VK_SKIP` | bitmask | skip selected Vulkan work, for A/B-ing a hypothesis instead of arguing about it. The full legend is printed at startup and lives next to the parser in `nvn_vulkan.c` (`1 无通道 2 无清除 4 无深…`); `16777216` forces all sampling to mip level 0, `1073741824` pins the present to the first source image, `2097152` never enables stencil test. Bits are claimed once they are used — check the legend before adding one (a collision here cost a round: `64` was already 呈现不用信号量) |
| `DM_ARENA_NODUP` | `0`/`1` | diagnostic only: `1` bypasses arena/vertex-buffer reuse; default `0`. Normal reuse now compares the full cached byte range before returning an existing copy |
| `DM_ARENA_ARM` | `0`/`1` | diagnostic only: re-arm guest buffer write protection; default `0` because earlier device runs froze with it enabled. Full-range comparison does not require it |
| `DM_VK_TEXTRACE` | `1` | opt-in bounded descriptor/upload correlation; default off; see [TEXTURE-DIAGNOSTICS.md](TEXTURE-DIAGNOSTICS.md) |
| `DM_VK_TEXTRACE_SERIAL` | serial or `0` | optionally restrict correlation to one logical texture serial; not a texture id |
| `DM_VK_TEXTRACE_FROM` | present count | optional start frame, default 0; budgets renew every 60 presents |
| `DM_TEXDUMP` | **absolute directory**, not `1` | Texture forensics, two artifacts per subresource: `…_A.bin` = the deswizzled **compressed blocks**, `…_B.bin` = what our CPU soft-decoder made of them (RGBA8). A correct B and a wrong A mean our geometry is wrong; a wrong A means the decode is. Round B38 wrote `1` here, saw all 16 log lines, and got an empty directory — the value is a path, and a relative one is reported and refused. Whole-image line too: `raw_s<serial>_v<stamp>.bin` is the block-linear store as the game wrote it, for brute-forcing per-level offsets offline. Caps: 400 pairs (1100 MB), 100 raw images (600 MB), 32 MB each. Must be outside `DM_SAVE`. See [P3-LOCALIZATION.md](P3-LOCALIZATION.md) |
| `DM_TEXDUMP_MIN` / `_MAX` | serial floor / ceiling | Aim the budget at one serial range. The ceiling stops the whole round once passed (serials are monotone) — do not set it above what this round reaches, and never use serials to compare two rounds |
| `DM_TEXDUMP_AT` | present count | Floor on frames presented instead: the world art is uploaded after the menu, and serials overlap between the two |
| `DM_TEXDUMP_LAYER` | layer index | Which layer of an array texture to dump (default 0). `A`/`B` are written for levels 0–1 of that layer only |
| `DM_RBIMG` | `0`/`1` | readback image capture (`rb_tga` of the present target) |
| `DM_TEXREAD` | absolute directory | after a texture is uploaded, copy the **VkImage itself** back off the GPU into this directory. This is the switch that distinguishes "the bytes landed wrong in the image" from "the bytes are right and the binding/UV is wrong". Must be outside `DM_SAVE`; output is capped (400 images, 4 per frame, 128 MB ring) |
| `DM_TEXREAD_MIN` | pixel count | ignore textures under this many pixels — serial numbers are per-process, so without a floor the budget is spent on menu icons before the world is entered |
| `DM_TEXREAD_LAYER` | layer index | Which layer to read back (default 0). Set it equal to `DM_TEXDUMP_LAYER` or the two sides never describe the same subresource |
| `DM_TEX_ASTC` | `1` | Offline BC→ASTC shortcut. Maleoon 920 samples ASTC 4×4 but no BC format, so the packaged art (NVN `0x44` = BC3) is normally CPU-decoded to RGBA8 on every upload. With `DM_TEX_ASTC=1` **and** the assets already transcoded on the build box (`scripts/astc_bake_all.sh` → `scripts/astc_swap.sh`), `fmt_of` maps `0x44` straight to `VK_FORMAT_ASTC_4x4_UNORM_BLOCK`: BC3 and ASTC 4×4 are both 4×4/16 B, so the block copy is verbatim, no deswizzle, no CPU decode, no 1→4 B/px upload inflation. Only takes effect when the device reports ASTC-4x4 support and *not* BC3; otherwise it logs why it did not arm. **The switch and the baked assets must move together** — an unconverted `.tex` read as ASTC is a garbled image. Readback of these lands as `rda_*` (still-blocked ciphertext; decode with `astcenc -dl` before comparing) |
| `DM_3D_SCALE`, `DM_3D_SCALE_DYN` | float | internal resolution scaling experiments |
| `DM_FLK` | mask | flicker/flat-fill detector — the line `[vk] 闪 …` fires on content change and reports brightness, source, and whether the texture was attached/uploaded/drawn |
| `DM_VK_KEYDUMP` | **absolute file path** | append every *new* shader stage key the guest binds, straight out of guest memory: one record = 32-byte header (key 16 / stage 4 / code address 8 / code length 4, little-endian) + the 0x880 control segment + the loader-patched instruction area. This is the input list for the offline SPIR-V build (`dm_shbin.py` → `dmsh_xlate` → `shbin_gen.py` → `shaders.bin`). Don't Starve passes **one stage per program** (`段数 1`), so the stage field in these records is always `0` — vertex vs fragment is decided later from the translated reflection, not here; do not read a column of zeros as a dump bug. The file is opened `"wb"`, so **a relaunch truncates it**: pull it before you relaunch, or the round's earlier keys are gone. **An empty file is a result, not a failure**: it proves the guest never reached program binding, so the stall is upstream of the shader library |
| `DM_SHBIN` | **absolute file path** | load the shader library from this file instead of the packaged one. Swapping `shaders.bin` this way needs **no HAP repackage** — `hdc file send` into the sandbox and relaunch, so a library iteration costs seconds against the 2.2 GB package. `shb_load` validates the whole table on open, so a corrupt library is refused loudly rather than half-working |
| `DM_RT_ASTC` | astcenc preset (`10`=FAST, `60`=MEDIUM) | on-device BC3→ASTC 4×4 at upload time (`nvn_rt_astc.c`) instead of shipping pre-baked art. Runs only when `DM_TEX_ASTC`/`romfs/data/astc_baked.txt` is disabled. Counters ride the draw line as `｜自转:档… 张… 块… 编码… ms 败… B2T…`; results are cached on disk (`DM_RT_ASTC_DIR`) so a warm run encodes 0 blocks. **Mutually exclusive with the A route on one install**: B must run against original BC3 bytes, otherwise the encoder is fed ASTC |

## Asking the game instead of averaging the frame

Aggregate counters answer "how much of everything"; on this title they repeatedly produced
confident, wrong conclusions (a `无库` percentage measured on a menu screen was read as an
in-world fact; a coverage percentage turned out to be one block repeated 40 times). These
switches exist to make the question per-object.

| Switch | Values | Meaning |
|---|---|---|
| `DM_SPKEY` | comma-separated program keys (8-hex prefix is enough) | **name the shader(s) you care about** and dump only the draws that use them. This is how "the text is missing" became "the font vertex stage is not in `shaders.bin`" — and how the answer was checked, because the same line reports the attribute layout the guest actually bound |
| `DM_SPFROM` | frame number | start the named dump at that frame, so a menu-time key does not spend the whole budget before the world is entered |
| `DM_SPN` | draws per (program, image) pair | how many times to print, per program — bounded on purpose, hilog silently drops a burst |
| `DM_SPDEC` | `1` | decode the captured buffers rather than hex-dumping them: `真值 属… 形… 类… 偏… 距…` for the vertex attributes and `常值 槽… 址… 长…` for the constants, plus `顶点色 …｜搬… ｜客…` which prints the host's copy and the guest's bytes side by side. Without this the readback is a pile of numbers you have to line up by hand |
| `DM_CBUF` | `1` | constant-buffer accounting on the draw line: `常量:陈旧 截断 全零 空槽 组`. `陈旧 > 0` would mean a stale copy; `截断` counts updates whose declared length exceeds the window — the arena hands the shader `size+256` bytes, mirroring NVN's read-past-the-window, so a non-zero `截断` alone is not a bug |
| `DM_ARENA_ARM` | `0`/`1` | `mprotect` the vertex arena read-only around the copy, so "the guest wrote after we moved it" becomes a fault with a count (`脏`/`臂`) instead of a theory |
| `DM_ARENA_NODUP` | `0`/`1` | turn arena dedup off for one round. Needed when testing whether dedup is what loses a write — leave it on otherwise, it is what keeps the 64 MB arena from filling |
| `DM_NVN_TSV` | file path | record the NVN call stream per frame into a tsv (`frame ⇥ name or key ⇥ count`) |
| `DM_NVN_TSV_CALLS` | `1` | in that tsv, write **per-API call counts** instead of only names. **Trap:** `cut -f2 \| sort \| uniq -c` counts *lines*, i.e. frames that emitted the name — one per frame, not the calls. Sum column 3 (`awk -F'\t' '$2=="nvnViewport"{s+=$3} END{print s}'`) or you will conclude the opposite of the truth. The offline case for the dynamic-state fix came out of this: 1,382,057 draws against 455k viewport and 488k scissor changes |

Two readouts that need no switch, because they are on the draw line: `｜缩绑:属 流 动发`
(how many rebinds shrank the attribute count, and how many dynamic-state re-emits happened) and
`｜清屏:模板 无面 全跳 色掩或 色掩与 色零` (what the guest's clear masks actually asked for).
`@draws` — the per-frame draw count — doubles as a scene fingerprint: 2 boot, 12–14 splash and
title, 40–44 submenu, 106–164 world-config modals, 651 roster, 750+ a live world.

## Audio and input

| Switch | Values | Meaning |
|---|---|---|
| `DM_AUDIO` | `0`/`1` | real OHAudio output vs the silent sink that still consumes buffers at the correct rate. `0` is useful when you want the guest's pacing unchanged but no sound |
| `DM_AUDIO_DUMP` | path | capture the PCM the guest produced |
| `DM_KBD` | text | short-circuit the software keyboard and return this string. Used for world and character names until the real input box is wired |
| `DM_HID_RECEIPT_LOG` | `1` | log every controller state receipt |

## Reading the output

The host's log lines are bilingual and terse by design; the statistics lines are the ones you will want to decode:

| Line | Meaning |
|---|---|
| `[vk] 上屏绑定 真N 哑M` | bindings on the on-screen pass: real / dummy |
| `[vk] 已画未上传绑定 N` | bindings with uploaded=0 AND drawn=1; includes normal render targets; not an empty-atlas count |
| `[vk] ｜打包:尝试图A 上B 重C 成MBD BCE 卡环F 越界G 解排H 格式I 深度J 三维K 环拒L 环成M 队满N 换位O 超表P 溢出Q 余不R 超容S 无环T 退避U 队龄V 重开W 串线X` | pack upload path. Every label is printed from one table entry that sits next to its own counter (`g_pkg_stat`), so a field cannot take its neighbour's number — the old hand-written argument list is what let round 40 publish a mis-read statistic. `余不` = no headroom left in this submission (retryable, and the only refusal that stops the whole flush), `超容` = this image is larger than the entire ring, `无环` = the ring could not be allocated, `退避` = skipped while parked in a backoff window, `队龄` = queued 32 generations without landing, `重开` = `sub_begin` re-entered while a buffer was still open, `串线` = begin and commit ran on different threads |
| `[vk] ｜哑因:没图A 空图B 深度非影C 影子给颜色D 强退哑图E｜影子真深度F 已画未上传绑定G｜采空时兄弟有货H 谁都没货I` | why a sample read a dummy: no image / empty texture / depth misuse / shadow-to-colour / forced dummy / shadow real depth / drawn-not-uploaded bindings / sibling had content at sample time / nobody did |
| `[vk] 源图 s<S> 图<I> 格<G> WxH 画过D 传过U` | one-time identity line per texture serial |
| `[vk] 程序首绑 <key> 绑定: …` | first binding set of each shader program, with `w/h/fmt/uploaded/drawn/bad/tried/range/L/pool` per image |
| `[vk] 排队上传 s=…` / `[vk] 槽全空 …` | the two older probes, capped at the first 40 and 24 events **per process**; `DM_VK_TEXTRACE=1` replaces them with windowed, renewing events |
| `svc=0x1c` on a sample line | that thread's last syscall — 0x1C/0x1D are the pre-4.0 semaphore wait/signal pair |
