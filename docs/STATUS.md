# Status — what works, what does not, and how far each branch is

Last measured on a real device: **2026-10-18**. Every number below came off a phone, not out of a model of what the code should do. Where a claim needs proof to be useful, the proof line is quoted.

## Where it stands

The game is entered and played: EULA → offline confirmation → real main menu → world list → world creation → **world generation completes** → character select (22 characters) → **in world**, day-1 HUD, inventory bar, interaction prompts. The one thing still wrong is the ground texture (flat green) — see [BLOCKERS.md](BLOCKERS.md).

Sampled in-world counters from a completed round:

```
[vk] 上屏绑定 真1682685 哑4737            bound on the on-screen pass: real / dummy
[vk] 上传像素有内容 235 张                 235 textures actually got non-zero pixels uploaded
[vk] 真绑但没传过 266425                   266,425 bindings referenced a texture that was never uploaded
[vk] 打包:排62 上65 成158 MB 卡arena246    pack queue: queued 62, uploaded 65, 158 MB, 246 arena rejections
```

World generation takes **2m20s** on the phone (`00:05:34` shard start → `00:07:54 Done forest map gen!`) against roughly 20–40 s on the original. That is the global-lock serialisation of SVCs, i.e. the performance branch, not a blocker.

## Branch completion

| Branch | Done | What the number is based on |
|---|---:|---|
| Packaging / install pipeline | **100%** | three commands, two self-checks, `bm install -p` for large HAPs; a round takes ~5 min |
| Forensics channel (logs, pixels, crashes, threads) | **100%** | sandbox readable, whole frames to files, `crash_<pid>.txt` and `thr.txt` complete |
| ELF load / SVC / threads / service stubs | **95%** | full chain to `nnMain` and stable; the TLS `_ZTH` family is still held up by stubs |
| File system and asset resolution | **92%** | the unresolved-reference file is never created; missing only an `lstat` fast path on the hot path |
| **Reaching the game** (EULA → menu → create world → in world) | **100%** | screenshotted; one script reproduces the whole path |
| Networking / the local shard | **80%** offline · **15%** online | real BSD loopback, RakNet connects; online is blocked by a missing TLS context — deliberately not pursued |
| Shader library / pipelines | **70%** | 222-key library, "no library" down from 41% to 10%; ~10% of keys still missing |
| **Texture upload chain** (memory pool → Vulkan) | **55%** | UI icons upload fine (235 with content); the terrain atlas never even enters the queue |
| Picture as a whole | **68%** | HUD/characters/grass/lighting/text correct; ground green (that is the row above) + stripe artefacts + a dark frame + 720p upscale blur |
| Input | **60%** | buttons reach the guest 100%; missing touch finger reporting, analogue stick/trigger values, rumble, ZR/R swap |
| Audio | **70%** | the OHAudio renderer is wired (ring buffer, zero-fill when empty, degrades to silence without stalling the game); **not yet verified scene by scene that sound actually comes out** |
| Stability (crashes) | **85%** | world-creation crash fixed, input-thread crash fixed, Break diagnostics hardened; long-run page-fault hook still under observation |
| Performance | **25%** | ~30 fps, p99 246 ms, worldgen 2m20s. Deliberately deferred: touching it every round pollutes every other measurement |
| Real text input box (swkbd) | **40%** | a `DM_KBD` bypass works for world/character names; the actual keyboard is not connected |

**Overall ≈ 62%.** Of the remaining 38%, the ground texture alone is about half the weight.

## Technical debt (does not block features, must be paid before "done")

- The lost-wakeup **root cause** is unfixed; a spurious-wake fallback (`DM_SPUR`) stands in for it. Removing that fallback today returns you to "stuck on starting server".
- Pack staging and the vertex stream share one 64 MB arena (`卡arena` went 0 → 479 → 246 across rounds).
- `nvn_dirty_since()` reports "dirty" for untracked ranges, so once the range table overflows (`MAX_RANGE 4096`, which has really been hit) that texture is re-uploaded on **every** bind. Fix: record a `tracked` bit at upload time and skip the dirty query when unset.
- No `lstat` fast path in the path mapper: the first lookup walks a whole directory.
- The HarmonyOS port layer (`dm/ohos/`, `dm/gfx/nvn/nvn_vulkan.c`, `hle_ipc.c`, `hle_stdio.c`) had **never been committed** until this repository was created. If you fork from here, that is the moment the history starts.
