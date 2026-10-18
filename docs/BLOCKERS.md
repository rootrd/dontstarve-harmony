# Blockers — evidence, dead ends, and the next move

Written so that whoever picks this up does not re-walk the paths that are already known to be wrong. Each entry: symptom → measurements → what was ruled out and how → next step.

---

## 1. The flat green ground (the only functional blocker)

**Symptom.** In world, everything renders except the terrain: grass tufts, the character, lighting, the day dial, the inventory bar, interaction labels ("常青树" = evergreen) are all correct; the ground is one flat green field.

**Historical measurements (not a confirmed root cause).**

The recorded run reported 266,425 `真绑但没传过` bindings and candidate images
`t311`/`t310` (2048×2048), `t302`/`t277` (1024×512), with `u0 d0 b0 t0 r0 m1 p1 L16384`.
Static review found that these are different observations and must not be conflated:

- `真绑但没传过` incremented only when `tex_has_content(v)` was true and `uploaded` was false.
  Since content means `uploaded || drawn`, this population has **drawn=1**. Render targets
  can legitimately contribute. The counter is now named `已画未上传绑定`.
- `程序首绑` lists occupied `S.tex` slots, not necessarily the shader's selected descriptors.
  It is capped at six slots per stage and 32 vertex-key identities.
- `打包:排` / `pkg_tried` was updated in `pkg_upload`, not `pkg_queue`. The counter is now
  `打包:尝试图`: distinct texture records that reached the upload function.
- Queue logs were limited to the first 40 events and empty-slot logs to the first 24 events
  in the process. Missing lines, especially after menu/world generation, prove nothing.

The historical missing-asset audit and mip-zero experiment remain useful prior evidence;
this patch does not repeat or change either experiment. The ground's root cause is still open.

**Ruled out (do not re-test these).**

| Hypothesis | How it was killed |
|---|---|
| Missing asset files | A full offline audit of all 21,310 files, then on-device: the unresolved-path list file is **never created**. Zero unresolved references. |
| Mips clamped so the atlas is sampled at an empty level | Ran a round forcing every sample to mip level 0 (`DM_VK_SKIP 16777216`): no change. |
| "No shader library" for these draws | The HUD and characters use the same library; the missing-library share is 10%, and it does not correlate with the terrain draws. |
| The pool memory is not mapped | `m1 p1` in the bind dump: memory and pool both present, `L16384` range registered. |
| The installed `shaders.bin` is too small and needs a full re-pack | Offline key-coverage check against the device-reported keys: the installed 40-entry library covers **all 27** of them, and the older 222/233-entry libraries share **zero** keys with the current chain (`e6ff54a2620e`) — they are a different key scheme, not a bigger version of this one. `SHB_MAXE` is 512, so headroom is not the constraint either. |

**Traps in this area, learned the hard way.**

- A combined NVN handle's **low 20 bits** are the texture id (`texh & 0xfffff`) — the same split `set_fill` uses. Passing the whole handle prints "(no image)" and sends you chasing a phantom.
- The program key lives on `progrec`, not `prog_t`; reaching for `g_cur_prog->key` from the wrong type is a compile error that wastes a build.
- A count in a stats line is not a population. `已画未上传绑定` (formerly `真绑但没传过`) only fires on the branch that requires `tex_has_content()`, so every one of those bindings had `drawn=1`; and the per-texture dumps are capped at the first 40 / 24 events **per process**, which the menu and world generation consume before the world is even entered.

**Next evidence: actual descriptors, correlated to upload events.**

See [TEXTURE-DIAGNOSTICS.md](TEXTURE-DIAGNOSTICS.md). `DM_VK_TEXTRACE=1` adds bounded,
renewing-window events with shader key, stage, descriptor binding, chosen NVN slot,
candidate handles, texture id/serial, and VkImage identity. Queue acceptance, refusal,
upload attempt and result are separate events. Rendering and slot fallback are unchanged.

Use `(serial,image)` to join descriptor and upload events within one resource lifetime;
`tid` is meaningful at descriptor selection and is `-1` in upload-only events. An image
handle may be reused after destruction, so frame/lifetime context still matters. Never
identify an atlas by dimensions alone. Rate-limited or missing events remain inconclusive.

If a correct selected descriptor queues but fails to record a copy, inspect packing. If
metadata says texture register T but the chosen fallback samples another nonempty slot,
compare the actual shader reflection and packed binding record before changing selection.
A missing shader draw also remains possible until the relevant shader key is established.

---

## 2. Lost wakeup (worked around, root cause unknown)

**Symptom (before the workaround).** The world never finished generating: the client's `global timeout 30000` fired, the shard tore down (`~ShardLuaProxy`), and no world files appeared.

**Evidence.** A thread dump showed `Server Master` and `Server Secondary` had each executed **exactly one SVC since boot** and were parked on a condition variable (cond key = 1, lock word = 0), while all 400 `SignalProcessWideKey` calls in the session never targeted their address. That is not slow generation — the thread was never woken.

**Workaround.** `DM_SPUR=<ms>`: every N milliseconds, give each kind-2 (condvar) waiter a spurious wake, and hand it the lock exactly as `0x1D` would — `lock_acquire_for(w, w->waddr, w->wtag)` ([hle_thread.c:164](../dm/host/hle_thread.c#L164)). A bare `wake()` is wrong: after `Wait` returns the guest continues **as the lock holder**. With this, `Server Master` went from 1 to 2.74 million SVCs and generation completed.

**Root cause still open.** The signal lands outside the waiter chain. The first 400 lines of both sides are already logged: `等待 key=… addr=… 超时=… thr=…` (0x1C) and `信号 key=… 个数=… 唤醒=… 余等待=…` (0x1D). Diff those two streams to find the address/tag that never meets. **Do not remove `DM_SPUR` before that is fixed.**

---

## 3. Pack staging vs. the vertex stream — code fixed, round pending

The old routing sent an image to the 64 MB arena when its size fitted `ARENA_PKG_CAP` (a third of
the arena) **without checking the arena's remaining headroom**, and to a one-shot 160 MB buffer
otherwise. The W24 round measured **`卡arena 539`, `巨图成 0`**: once dirty-page re-uploads and the vertex
stream had eaten the arena, the 8–16 MB terrain atlases (`s311`/`t310`/`t277`/`t302`) were all routed
onto a full arena and died on the first `stage_take` — and because exhaustion was treated as a hard
failure, `pkg_bad` was set and those textures were never retried. Terrain drew `u0` flat green.

Now in the tree: packaged staging always uses the dedicated 160 MB ring (`BIG_STAGE_SZ`), the arena
keeps only the vertex stream and `BufferToTexture`, and `stage_begin` returns four distinct outcomes
instead of a bool — `STG_HEAD` (this submission has no room left: stop the batch, retry when the
cursor resets), `STG_OVER` (this one image is bigger than the whole ring), `STG_NOBUF` (the ring could
not be allocated at all), `STG_OK`. None of them may become a permanent `pkg_bad`: `pkg_backoff` parks
the texture for `1 << min(rej, 6)` submissions (x16 for `STG_OVER`/`STG_NOBUF`) and records why in
`pkg_why`, so a transient refusal costs a delay, not the texture. `pkg_flush` additionally caps one
pass at 256 textures and applies a fairness valve (age >= 128 drops a ticket so it cannot starve the
rest of the queue), keeps the un-flushed tail on refusal, and every size computation goes through
`pkg_mul_ok` / `pkg_add_ok` so an overflow is counted (`ovfl`) and fatal instead of silently wrapping.
Whole-image-or-nothing is unchanged: a texture reserves its full footprint or moves nothing.
Stats moved to a 24-entry label table (`g_pkg_stat`), printed as `head=… over=… nobuf=… bkoff=…`
per item — read the label of the field you quote, the previous `卡环 / 环拒 / 环成` line is gone.

**Host receipt for this batch** — reproducible with `scripts/pkg_faults.sh` (it `awk`-extracts the
production bodies out of `nvn_vulkan.c` verbatim, compiles them against the injectable stubs in
`dm/gfx/nvn/tests/pkg_faults.c`, and runs the matrix on the host): 176 assertions, 0 red under
ASan+UBSan; `NEG=6a4def6 scripts/pkg_faults.sh` runs the same assertions against the pre-fix commit and
gives 125 assertions, 46 red, so the tests demonstrably bite the defects rather than the stubs.
`make -f dm/ohos/Makefile sweep` = `ok=89 fail=0`, and
`-Werror=format -Werror=format-extra-args` is clean.

What the next device round has to answer: whether `bigok` now counts the terrain atlases through, and
whether `head` stays near zero. If `head` climbs instead, the ring is the constraint and the fix is
the per-submission cursor policy, not the decode path. Related: the `pkg_tracked` item in
[STATUS.md](STATUS.md#technical-debt-does-not-block-features-must-be-paid-before-done).

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
