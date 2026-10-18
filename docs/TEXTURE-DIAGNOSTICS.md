# Texture diagnosis patch: interpretation and safe verification

Baseline the patch was written against: `5bf14e17735d32efd7b93c827462a87e78e0a053`. It was merged
on 2026-10-18 into the tree that matches the build actually running on the phone, which is ahead of
that baseline in the pack-staging area (see the merge notes below), so `nvn_vulkan.c` was 3-way
merged rather than applied.

## Merge notes (what differs from the patch as authored)

- Packaged staging no longer routes by static size: `ARENA_PKG_CAP`, the `big` flag and the
  arena/huge-buffer split are gone — every packaged upload takes its whole footprint from a
  dedicated 160 MB ring (`BIG_STAGE_SZ`), and ring exhaustion is retryable rather than a permanent
  `pkg_bad`. The patch's `打包` stats hunk still carried the old arena labels; the merged line uses
  `卡环 / 环拒 / 环成`, and its counter was renamed to `g_pkg_attempted_textures` as the patch asked.
- The patch's own version of that stats line had **13 conversion specifiers and 14 arguments** —
  `(g_pkg_bytes >> 20)` was passed twice, so `BC层` printed MB and every later field shifted by one.
  That is where the published "`卡arena` 246" reading came from (246 was `BC层`). Fixed during the
  merge; `clang -Wformat=2` is now clean on the file except the pre-existing
  `-Wformat-nonliteral` note on the shared log formatter.
- Two counter renames kept: `g_b_neverup` → `g_b_drawn_not_uploaded` (`已画未上传绑定`) and
  `g_pkg_q` → `g_pkg_attempted_textures` (`尝试图`), including the `哑因` line.

## What changed

1. Clamp file-log writes to the bytes actually present after `vsnprintf` truncation.
   Previously a long log could read beyond the 1200-byte stack buffer.
2. Validate shader ranges with subtraction, avoiding unsigned addition wraparound.
   Reject invalid stages and misaligned SPIR-V sections. Validate all entries before
   publishing pointers/index entries; failed lookup cannot read an unmapped library.
3. Correct misleading counters and add optional descriptor/upload correlation.

This is **not a verified fix for the green terrain**. This patch changes no texture slot fallback,
decode path, rendering, shader binary format version, or guest resource; the staging-allocation
change described above was already in the device build before the patch was merged, and is not part
of what this patch verified.
The loader checks reject malformed libraries; they do not prove that valid metadata is
semantically correct. The packing tool and the user's actual `shaders.bin` still need
inspection for that question.

## Trace switches

Add to the existing, approved launch configuration (do not replace its game/save options):

```text
--ps DM_VK_TEXTRACE 1
--ps DM_LOGFILE <app-writable diagnostics path outside the save directory>
```

Optionally add `--ps DM_VK_TEXTRACE_SERIAL <serial>` or
`--ps DM_VK_TEXTRACE_FROM <present-count>`. Serial is the logged logical identity, **not**
`t311` or a guessed atlas size. Zero means no serial filter. Invalid numeric filters disable
tracing. All values are read once per process. Default tracing is off.

Each 60-present window permits at most 12 distinct signatures for each of five event types
(60 lines total), plus a rollover summary. Duplicate/capacity-suppressed events are counted.
Budgets renew after startup; they do not get permanently exhausted in the menu. This is
bounded sampling, not an exhaustive event journal. A last window may end before its summary
is printed; absence never proves non-execution. For a crowded window, narrow the serial filter.

- `descriptor`: actual image descriptor selected by `set_fill`. Contains shader key, stage,
  SPIR-V binding, NVN kind/bank/reg, chosen slot/source, combined handle, all three candidate
  handles, and whether the final view is a dummy. These are descriptor-build events, not
  per-draw counts; cached descriptor sets can be reused without another event.
- `queue`: accepted by `pkg_queue`.
- `attempt`: entered `pkg_upload` outside a render pass with an image.
- `result`: `copy-recorded`, `bad`, or `deferred-or-incomplete`. Copy recorded means Vulkan
  copy commands were recorded, **not** that GPU completion or correct pixels were verified.
- `drop`: queue/flush refusal reason. The queue's existing pointer-compaction behavior is
  unchanged; changes of image identity between queue and flush are useful evidence.

Join by serial **and** image identity, preserving frame order. Different VkImages can share
a serial; VkImage handles can be reused after destruction. Upload-only records use `tid=-1`
because the queue has no unique NVN descriptor id. Candidate handles use the low 20 bits as
texture id. `u/d/bad/tried` report upload/draw/failure/attempt state at the event.

Do not alter bank/binding/reg fallback based only on a missing trace. Compare the selected
shader's reflection JSON, the actual packed `shb_bind`, and the occupied NVN slots first.

## Local tests (no SDK or game required)

From the repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic dm/gfx/tests/t_nvn_diag.c -o /tmp/t_nvn_diag
/tmp/t_nvn_diag
python3 dm/gfx/tests/test_nvn_diag_integration.py
SANITIZE=1 ASAN_OPTIONS=detect_leaks=0 python3 dm/gfx/tests/test_nvn_diag_integration.py
```

The integration script extracts the exact production logger, trace, and shader-loader
functions into a host harness with logging/platform stubs. It checks long-log truncation,
sibling-image trace identity, serial filtering/window renewal, valid lookup, and a malformed
second shader entry followed by repeated lookup. It does not compile the whole Vulkan file
or prove an OHOS build. Leak detection is disabled only because the test executor is ptraced;
AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.

Verified on the merged tree (2026-10-18, Linux build box, OHOS sysroot):

- `make -f dm/ohos/Makefile sweep` → `ok=89 fail=0`; `solib` links `libdmhost.so`.
- `clang --target=aarch64-linux-ohos -Wformat=2` on `nvn_vulkan.c` → no format/argument mismatch.
- Both host suites pass, plain and under `-fsanitize=address,undefined`, against the merged
  `nvn_vulkan.c` (the extraction markers the integration test slices on still line up).
- `strings libdmhost.so | grep -c '\[textrace\]'` → 19, so the tracing is in the shipped library.
- **No device round has been run with tracing on yet.** Every statement above is about compilation
  and host-level behaviour, not about what the phone prints.

## Loader-gate safety, checked offline

The stricter `shb_load()` gates reject a library the old loader would have accepted, so before
merging them every `shaders.bin` produced by this project was replayed through the new predicates
off-device (header magic/version/size, the two alignment checks, and the per-entry range and
`stage > 2` checks). **Nothing that was ever packed is newly rejected**: each library validates
entry-for-entry, so the loader change cannot be the reason shader replay stops working.

That same inspection closed the "is the installed library too small" question: the one shipped in
today's `resfile` holds 40 entries and covers **all 27** keys the device reported in the sampled
round, while the older 222- and 233-entry libraries share no keys with the current chain
(`e6ff54a2620e`) — they belong to the superseded key scheme, so there is nothing to re-pack.
`SHB_MAXE` is 512, roughly 12× the entries in use.

## Deployment boundary

Use the existing trusted OHOS toolchain to build `libdmhost.so`; do not download game data
or run an unfamiliar binary for this patch. Before packaging, verify the intended git base,
review the diff, build all changed code, and check the packaged library contains `[textrace]`.

**Do not run `scripts/pack_install.sh` as-is:** it uninstalls the app. Preserve and verify a
backup of existing saves and diagnostics before any installation. Prefer a platform-supported
same-bundle, same-signature update that retains data; verify that route on the actual device
and tooling rather than assuming success. If an update is refused, stop and diagnose; do not
fall back to uninstalling. Keep the existing bundle identity, signing material and data modules.

Do not run `scripts/run_world.sh` for this verification: it auto-accepts terms, creates a
world, and uses fixed coordinates/timers. Enter the existing test world manually after the
user's normal login/terms steps. Preserve `DM_TITLE_DRV=0` and the current `DM_SPUR` workaround.
Collect logs/screenshots without changing saves. No device deployment was performed here.
