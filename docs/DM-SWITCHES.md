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
| `DM_SAMPLE` | **milliseconds** | periodic sampler line, incl. the last SVC number per thread. `0` means "every 0 ms" — 6,000 lines/second, which evicts your own diagnostics from the log. **Use 2000** |
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
| `DM_VK_SKIP` | bitmask | skip selected Vulkan work — used to force all sampling to mip level 0 when testing texture hypotheses |
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
| `DM_3D_SCALE`, `DM_3D_SCALE_DYN` | float | internal resolution scaling experiments |
| `DM_FLK` | mask | flicker/flat-fill detector — the line `[vk] 闪 …` fires on content change and reports brightness, source, and whether the texture was attached/uploaded/drawn |

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
