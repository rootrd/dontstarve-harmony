# Blockers — evidence, dead ends, and the next move

Written so that whoever picks this up does not re-walk the paths that are already known to be wrong. Each entry: symptom → measurements → what was ruled out and how → next step.

---

## 1. The flat green ground (the only functional blocker)

**Symptom.** In world, everything renders except the terrain: grass tufts, the character, lighting, the day dial, the inventory bar, interaction labels ("常青树" = evergreen) are all correct; the ground is one flat green field.

**Measurements.**

- The terrain atlases are `t311`/`t310` (2048×2048) and `t302`/`t277` (1024×512). At bind time each reports `u0 d0 b0 t0 r0 m1 p1 L16384`: never uploaded, never drawn, never queued for packing, never attempted — but the pool memory and the range **are** present.
- Across a whole in-world round: `真绑但没传过 266425` (266,425 binds of textures that were never uploaded) against `上传像素有内容 235 张` (235 that really got pixels).
- The pack queue for the same round: `排62 上65` — 62 queued, 65 uploaded. So the terrain atlas is not "queued and lost", it is **never queued**.

**Ruled out (do not re-test these).**

| Hypothesis | How it was killed |
|---|---|
| Missing asset files | A full offline audit of all 21,310 files, then on-device: the unresolved-path list file is **never created**. Zero unresolved references. |
| Mips clamped so the atlas is sampled at an empty level | Ran a round forcing every sample to mip level 0 (`DM_VK_SKIP 16777216`): no change. |
| "No shader library" for these draws | The HUD and characters use the same library; the missing-library share is 10%, and it does not correlate with the terrain draws. |
| The pool memory is not mapped | `m1 p1` in the bind dump: memory and pool both present, `L16384` range registered. |

**The two-way split, already coded.** Two probes are in the tree:

- `[vk] 排队上传 s=… %ux%u 格… 级… 传过…` in `pkg_queue()` ([nvn_vulkan.c:4851](../dm/gfx/nvn/nvn_vulkan.c#L4851), first 40) — *does the terrain atlas ever enter the upload queue?*
- `[vk] 槽全空 key0=… st… bank=… binding=… reg=…｜槽内 b=… d=… r=…` in the all-candidates-empty branch of `set_fill()` ([nvn_vulkan.c:3959](../dm/gfx/nvn/nvn_vulkan.c#L3959), first 24) — *does the shader even look at the slot where the handle sits?*

Reading the result:

- If a 2048×2048 / 1024×512 image **appears** in `排队上传`, the queue is entered and the loss is downstream: read the counters `打包:排/上/成/卡arena/BC层/格式` and follow `stage_take` rejections and decode failures.
- If there is **no** `排队上传` line but there **are** `槽全空` lines, the descriptor is being filled from the wrong bank/binding/register. Compare against the `程序首绑` line ([nvn_vulkan.c:4187](../dm/gfx/nvn/nvn_vulkan.c#L4187)), which dumps the first six bound images per program with `w/h/fmt/uploaded/drawn/pkg_bad/pkg_tried/range/L/pool`.

**Traps in this area, learned the hard way.**

- A combined NVN handle's **low 20 bits** are the texture id (`texh & 0xfffff`) — the same split `set_fill` uses. Passing the whole handle prints "(no image)" and sends you chasing a phantom.
- The program key is on `progrec`, not `prog_t`; reaching for `g_cur_prog->key` from the wrong type is a compile error that wastes a build.
- The `没传过就绑` probe ([nvn_vulkan.c:4028](../dm/gfx/nvn/nvn_vulkan.c#L4028)) already exists and prints the first 24 such textures with tid/size/format/serial — start there before adding anything new.

---

## 2. Lost wakeup (worked around, root cause unknown)

**Symptom (before the workaround).** The world never finished generating: the client's `global timeout 30000` fired, the shard tore down (`~ShardLuaProxy`), and no world files appeared.

**Evidence.** A thread dump showed `Server Master` and `Server Secondary` had each executed **exactly one SVC since boot** and were parked on a condition variable (cond key = 1, lock word = 0), while all 400 `SignalProcessWideKey` calls in the session never targeted their address. That is not slow generation — the thread was never woken.

**Workaround.** `DM_SPUR=<ms>`: every N milliseconds, give each kind-2 (condvar) waiter a spurious wake, and hand it the lock exactly as `0x1D` would — `lock_acquire_for(w, w->waddr, w->wtag)` ([hle_thread.c:164](../dm/host/hle_thread.c#L164)). A bare `wake()` is wrong: after `Wait` returns the guest continues **as the lock holder**. With this, `Server Master` went from 1 to 2.74 million SVCs and generation completed.

**Root cause still open.** The signal lands outside the waiter chain. The first 400 lines of both sides are already logged: `等待 key=… addr=… 超时=… thr=…` (0x1C) and `信号 key=… 个数=… 唤醒=… 余等待=…` (0x1D). Diff those two streams to find the address/tag that never meets. **Do not remove `DM_SPUR` before that is fixed.**

---

## 3. `卡arena` — pack staging fights the vertex stream

`卡arena` (arena rejections) went **0 → 479 → 246** across rounds. Re-upload and the vertex stream share one 64 MB arena, so a single submission containing a few 16 MB-class images exhausts the staging buffer and `stage_take` is refused. Fix: give texture packing its own ring. Related: see the `nvn_dirty_since()` item in [STATUS.md](STATUS.md#technical-debt-does-not-block-features-must-be-paid-before-done).

---

## 4. Performance

~30 fps average (`出图 10021 帧 / 330 s`), **p99 246–254 ms** frame spikes, ~4 GB cumulative software texture decode, worldgen 2m20s vs 20–40 s originally (the global lock serialises SVCs). **Deliberately last**: every round that touches performance invalidates the judgement criteria of every other branch.

---

## 5. Input: what is actually missing

Buttons reached the guest a long time ago (the world-creation screen in this project's history was driven forward purely by injected key events). What is genuinely missing: **touch → NVN finger reporting** (needed for phone-style play), analogue stick and trigger values, rumble, and the ZR/R swap left over from the on-screen layout. Do not treat "input" as the next milestone — it is a short list of four items, not a wall.

---

## 6. Real text input (swkbd)

`DM_KBD=<text>` short-circuits `ShowVirtualKeyboardThread` and is how world and character names get filled today. It works; it is not a keyboard. Wiring the real one is a bounded task, plus verifying the string is written back into the guest buffer.

---

## 7. Remaining picture defects (to be settled together with #1)

- Vertical stripe ("barcode") artefacts on the EULA page and some panels.
- Some button labels missing.
- The whole frame reads slightly dark.
- **Text blur is not a bug**: the game renders 1280×720 and the swap chain is 2848×1276, so the present blit upscales 2.22× with a LINEAR magnification filter. The discriminator: in the same screenshot, our own native-resolution debug overlay text is crisp while the game's text is soft. Switching the present to NEAREST gives hard edges and shimmer — a taste call, not a fix.

---

## 8. Deliberately not pursued

`无法连接到科雷服务器` ("cannot connect to the Klei server") traces to the guest's `Failed to create the SSL context` — the host provides no TLS. It blocks **online play only**; offline single-player is unaffected. Opening a branch for it is wasted effort unless online play becomes a goal.

---

## Appendix: the causal chains that took the most rounds to untangle

Recorded because each one looked like something else at first.

1. **Crash on "+":** the guest raised its own SDK assertion (`Break`, reason 0, size 4). Our *diagnostic* then dereferenced a wild pointer inside the Break handler and its own SIGSEGV **masked** the guest's abort → added `mincore` page checks and bounded reads before anything else could be seen.
2. **Bare asset names:** the game asks for `rom://cave.xml`, `cave.tex`, `map_edge.*`, `ocean_noise.tex`, `mini_water_*` with no directory, while the files live under `levels/tiles/` and `levels/textures/` → a bare-name fallback, then a full basename index.
3. **`SetCpuBoostMode` → null service proxy → world creation aborts:** the SDK path goes `applet::SetCpuBoostMode` → `am::GetCommonStateGetter()`; the host's `am` proxy is always NULL, so `ldr x8,[x0]` with `x0=0`. The host does not need CPU boosting.
4. **"Stuck on starting server" = a real socket is required:** DST's single-player mode is an in-process dedicated server (`instance_2`/`instance_3`), configured under `savedata_1:/SAVE/DoNotStarveTogether/Cluster_N/` with `bind_ip=127.0.0.1`, ports 10888/10999. The old socket layer returned `ENETDOWN` for everything.
5. **The D2R input drivers crashed DST's input thread:** `dm_cursor_apply()` sat **outside** the `DM_TITLE_DRV` gate and read the guest object through a Diablo II offset (`addr=0x58`). Rule: every "index into the guest object by b+offset" call in the gamepad path must be inside that gate.
