# Status — what works, what does not, and how far each branch is

Last measured on a real device: **2026-10-18**. Every number below came off a phone, not out of a model of what the code should do. Where a claim needs proof to be useful, the proof line is quoted.

## Where it stands

The game is entered and played: EULA → offline confirmation → real main menu → world list → world creation → **world generation completes** → character select (22 characters) → **in world**, day-1 HUD, inventory bar, interaction prompts. The one thing still wrong is the ground texture (flat green) — see [BLOCKERS.md](BLOCKERS.md).

Sampled in-world counters from a completed round. **Read the field list of the line you are
quoting** — the labels were renamed once, and the published numbers below were read against the
old labels:

```
[vk] 上屏绑定 真1682685 哑4737            bound on the on-screen pass: real / dummy
[vk] 上传像素有内容 235 张                 235 textures actually got non-zero pixels uploaded
[vk] 已画未上传绑定 266425                 (was 真绑但没传过) bindings whose texture had drawn=1
                                         but uploaded=0 — render targets count here, so this is
                                         NOT evidence of an uninitialized atlas
[vk] 打包:尝试图62 上65 重3 成158 MB BC层246 卡环0 ...
                                         distinct textures that reached pkg_upload / uploads /
                                         re-uploads / bytes / BC decode layers / ring refusals
```

The `246` in the old line was `BC层` (soft-decode layer count), not arena rejections; `卡arena`
was 0 in that round. The pack line has since been rebuilt as a 24-entry label table (see
[DM-SWITCHES.md](DM-SWITCHES.md#reading-the-output) for the current field list), so **do not decode a
round taken before that change with today's labels, or a round taken after it with the sample above**.
[BLOCKERS.md](BLOCKERS.md#1-the-flat-green-ground-the-only-functional-blocker)
lists what each probe can and cannot prove.

World generation takes **2m20s** on the phone (`00:05:34` shard start → `00:07:54 Done forest map gen!`) against roughly 20–40 s on the original. That is the global-lock serialisation of SVCs, i.e. the performance branch, not a blocker.

## Branch completion

| Branch | Done | What the number is based on |
|---|---:|---|
| Packaging / install pipeline | **100%** | three commands, two self-checks, `bm install -p` for large HAPs; a round takes ~5 min |
| Forensics channel (logs, pixels, crashes, threads) | **100%** | sandbox readable, whole frames to files, `crash_<pid>.txt` and `thr.txt` complete |
| ELF load / SVC / threads / service stubs | **95%** | full chain to `nnMain` and stable; `_ZTH` now hands each guest module a real TLS block and the guest exception stack is a host buffer |
| File system and asset resolution | **92%** | the unresolved-reference file is never created; missing only an `lstat` fast path on the hot path |
| **Reaching the game** (EULA → menu → create world → in world) | **100%** | screenshotted; one script reproduces the whole path |
| Networking / the local shard | **80%** offline · **15%** online | real BSD loopback, RakNet connects; online is blocked by a missing TLS context — deliberately not pursued |
| Shader library / pipelines | **70%** | the installed library (chain `e6ff54a2620e`) holds 40 entries and covers all 27 keys the device reported in that round; the 41%→10% "no library" figures belong to the older key scheme |
| **Texture upload chain** (memory pool → Vulkan) | **55%** | UI icons upload fine (235 with content); the terrain atlas is not confirmed either way — the queue log that suggested it was capped at 40 events |
| Picture as a whole | **68%** | HUD/characters/grass/lighting/text correct; ground green (that is the row above) + stripe artefacts + a dark frame + 720p upscale blur |
| Input | **60%** | buttons reach the guest 100%; missing touch finger reporting, analogue stick/trigger values, rumble, ZR/R swap |
| Audio | **70%** | the OHAudio renderer is wired (ring buffer, zero-fill when empty, degrades to silence without stalling the game); **not yet verified scene by scene that sound actually comes out** |
| Stability (crashes) | **85%** | world-creation crash fixed, input-thread crash fixed, Break diagnostics hardened; long-run page-fault hook still under observation |
| Performance | **25%** | ~30 fps, p99 246 ms, worldgen 2m20s. Deliberately deferred: touching it every round pollutes every other measurement |
| Real text input box (swkbd) | **40%** | a `DM_KBD` bypass works for world/character names; the actual keyboard is not connected |

**Overall ≈ 62%.** Of the remaining 38%, the ground texture alone is about half the weight.

## Technical debt (does not block features, must be paid before "done")

- The lost-wakeup **root cause** is unfixed; a spurious-wake fallback (`DM_SPUR`) stands in for it. Removing that fallback today returns you to "stuck on starting server".
- Pack staging **solved in the tree, not yet device-measured**: packaged textures stage on their own 160 MB ring (`BIG_STAGE_SZ`), the arena keeps only the vertex stream and `BufferToTexture`, and a refusal is now one of four graded outcomes (`STG_HEAD` / `STG_OVER` / `STG_NOBUF` / `STG_OK`) with a backoff window behind it — none of them may set `pkg_bad`, so a texture that does not fit this submission waits instead of being written off forever. Size arithmetic is checked (`pkg_mul_ok` / `pkg_add_ok`); an overflow is a counted FATAL, never a wrapped number. The old failure mode was pack staging and the vertex stream sharing one 64 MB arena, and the old code treated any refusal as permanent.
- Upload request identity and lifetime **solved in the tree, same caveat**: the queue holds a ticket (pointer + serial + image handle + generation it was born in), so a swap-remove in `g_tex` cannot make one texture's request land on another; a revive resets the previous life's upload accounting; `tex_release` hands handles to a deferred-destroy graveyard and only frees them once a submission has passed them. "Success" now means every mip and layer was reserved and copied — a partial plan records nothing.
- Evidence chain **solved in the tree, same caveat**: readback got the missing layout transitions/ barriers and a per-chunk TGA offset fix, the pack statistics are label-adjacent, and `DM_TEXREAD` reads an uploaded `VkImage` back off the GPU so "bytes landed wrong" and "binding/UV wrong" are separable on the phone.
- Verified offline, not yet on the phone: the three items above were checked by a fault-injection harness (`scripts/pkg_faults.sh` + `dm/gfx/nvn/tests/pkg_faults.c`) that `awk`-extracts the production bodies verbatim and runs them against stubs on the host — **176 assertions, 0 red** under ASan+UBSan, and the same assertions against the pre-fix commit `6a4def6` give **46 red**, so they bite the defects. Details in [BLOCKERS.md](BLOCKERS.md#3-pack-staging-vs-the-vertex-stream--code-fixed-round-pending).
- Untracked-range dirty reporting **solved in the tree, same caveat**: `pkg_tracked` records at upload whether the pool range is inside the dirty table, and the bind-time check skips untracked ranges. With `MAX_RANGE 4096` really being hit, the old behaviour meant re-uploading on every bind.
- No `lstat` fast path in the path mapper: the first lookup walks a whole directory.
- The HarmonyOS port layer (`dm/ohos/`, `dm/gfx/nvn/nvn_vulkan.c`, `hle_ipc.c`, `hle_stdio.c`) had **never been committed** until this repository was created. If you fork from here, that is the moment the history starts. Until 2026-10-18 the published file was an older snapshot than the build that runs on the phone; the tree now matches the device build modulo comment sanitisation.
