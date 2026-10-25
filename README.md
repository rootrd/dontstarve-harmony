# Don't Starve Together (Switch) running natively on HarmonyOS — an HLE port

**Don't Starve Together**, the Nintendo Switch build, executed **natively** on a Huawei HarmonyOS phone. Not an emulator shell, not a remake, not cloud streaming: the guest's ARM64 machine code runs inside our HLE (high-level emulation) host, and every platform capability it asks for — threads and semaphores, file system, sockets, gamepad, audio, and the **NVN graphics API** — is served by a native HarmonyOS implementation. NVN is replayed **call by call into Vulkan** and drawn on a Maleoon 920.

**Current status (2026-10-24, second device):** the same build was re-checked on a **Maleoon 920** and confirmed end-to-end — menus, the Chinese world-settings screen, the roster and a live world all render, and the Florid Postern is clean. The regression reported against this library the day before was **our own sampling error** (one screenshot taken 30 s into a night-time world) and has been retracted; see [STATUS](docs/STATUS.md). Carried over from the 2026-10-23 repair round: arena alignment, mutable-buffer reuse and out-of-range Vulkan vertex bindings are fixed, and startup plus the Chinese world-settings screens align correctly. This build uses 105 regenerated shader stages and software decoding of the original BC assets — and our own toolchain now reproduces that 105-stage library byte for byte from the packaged ROM assets. See the **[repair report](docs/OHOS-RENDERING-2026-10.md)** for the changes, tests and remaining validation. The screenshots and round counters below document the earlier AN build.

<p align="center">
  <img src="docs/screens/title_before_an.jpg" width="49%" alt="Title screen before: the whole figure is scattered into triangles">
  <img src="docs/screens/title_after_an.jpg" width="49%" alt="Title screen after: Wilson, the hammer and the composition are coherent, some shards remain">
</p>
<p align="center"><sub>Left: the title screen before this week's state work (the AK1 build). Right: the same screen on the historical AN build — the figure, hammer and banner are coherent; the shards on the right edge are what is still open. Same device, same packaging chain, no per-screen special cases.</sub></p>
<p align="center">
  <img src="docs/screens/main_menu.jpg" width="49%" alt="The real main menu: Wilson, the menu column, the offline badge">
  <img src="docs/screens/in_world.jpg" width="49%" alt="In the world: day counter, inventory bar, HUD and subtitle text render; a built structure in the middle is still shredded">
</p>
<p align="center"><sub>Left: the main menu renders complete. Right: actually in the world — day 1, the inventory bar, the stat orbs and a spoken subtitle are all there; the structure in the middle is the tearing above.</sub></p>

Platform: real Huawei device (HarmonyOS NEXT, Maleoon 920, 2848×1276) · Guest: the Switch release of Don't Starve Together · License: MIT · **Bring your own game files — this repository contains no game content**

中文: [README.zh-CN.md](README.zh-CN.md) · Progress: [docs/STATUS.md](docs/STATUS.md) · Blockers: [docs/BLOCKERS.md](docs/BLOCKERS.md) · Getting the game files: [docs/UNPACKING.md](docs/UNPACKING.md)

---

## Why this route

There are two ways to run a Switch game elsewhere: build a full Horizon OS emulator (CPU JIT, the whole service set, a shader compiler), or — if the game touches only a thin slice of the platform — treat it as a dynamic library with a different target ABI and implement the syscalls and APIs by hand. Don't Starve Together is the second case: its graphics go through NVN, which maps mechanically onto Vulkan; its platform needs concentrate in a small set of `nn::*` services; and its data (romfs) can be read as-is.

The same host previously got the Switch release of *Diablo II: Resurrected* running on iOS ([d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), same author, MIT). This repository is its **HarmonyOS branch**: the host layer is reused, what is new is the HarmonyOS port (NAPI / XComponent / OHAudio / touch) and the **NVN→Vulkan replay layer** (`dm/gfx/nvn/nvn_vulkan.c`).

## How the data flows

```
hdc aa start --ps DM_xxx  ──►  ArkTS entry (EntryAbility) hands the parameters to NAPI
                                 ─► libdmhost.so: loads main / sdk / subsdk0 (ELF + instruction-level patch tables)
                                     ├─ SVC        hle_svc.c      incl. the pre-4.0 semaphore family 0x1A–0x1D
                                     ├─ threads    hle_thread.c   condvars / spurious-wake fallback
                                     ├─ file system hle_fs.c      romfs mount + 3-tier path fallback + lazy basename index
                                     ├─ network    hle_net.c      real BSD sockets (the game's own local shard needs them)
                                     ├─ input      hle_hid.c      on-screen Switch layout / touch → Npad
                                     ├─ audio      hle_audio.c + ohos/audio_ohos.c (OHAudio)
                                     └─ graphics   gfx/nvn/nvn_vulkan.c   NVN calls replayed into Vulkan
                                          └─ memory pool → pack queue → BC/ASTC software decode → staging → vkUploadTexture
```

One property earns its keep: **a guest module's file offset equals its memory offset, 1:1**. So any crash address `pc=<module>+<offset>` can be resolved offline against the ELF's `.dynamic` / `.dynsym` / `.rela.plt` — no debugger on the device. That turns "run a round on the phone, then name the exact symbol" into a five-minute step.

## What is already solved (the ones that save hours)

The full list with the measurements that justify each entry is in [docs/BLOCKERS.md](docs/BLOCKERS.md) and [docs/STATUS.md](docs/STATUS.md). The highlights:

- **The whole startup chain**: `nnosInitialize` → SDK module init → global constructors of three modules (301 + 112 + 45) → `nnMain` → romfs mount → 12+ guest threads → audio output open.
- **A batch of missing services**: eShop, the Joy-Con-detached family, the default-audio-out overload, `nn::oe::SetCpuBoostMode`, `nn::pctl::IsFreeCommunicationAvailable`. Shared mechanism: the guest's `sm::Initialize()` cannot connect, the SDK reads an invalid response and `diag::AbortImpl` kills the process. **Stubs are only added to symbols the main program does not reference** — zero risk to the existing baseline.
- **NVN→Vulkan replay**: 18 entry points the guest wanted and the host lacked; viewport Y-flip (with the scissor box following) fixed the vertical mirror; **the real cause of flat-fill UI was a mis-indexed NVN→Vk blend factor table**. The shader library went 96 → 222 keys while "no library" fell from 41% to 10%, and the key scheme was then rebuilt so a library survives a re-pack. The historical AN library was 115 entries / 1.2 MB (the current 1.30.0 rebuild has 105 stages; see the repair report) and includes the **font vertex stage** (entry 114) — found by naming one key and dumping only the draws that use it (`DM_SPKEY` / `DM_SPFROM` / `DM_SPN` / `DM_SPDEC`), which is also how "the text is missing" turned out to be a missing shader rather than a missing asset.
- **State fidelity, this week**: the replay layer now honours the things it used to drop — a vertex rebind that *shrinks* the attribute count, the attribute count itself as part of the pipeline key, dynamic state re-emitted whenever the guest changes it rather than only on a pipeline swap (plus the three stencil states declared dynamic), and the `depth_mask` / `stencil` / `stencil_mask` of a depth-stencil clear, with the stencil aspect only cleared on formats that really have one.
- **Texture upload chain**: packaged textures stage on their own 160 MB ring instead of fighting the vertex stream for the 64 MB arena, dirty-page re-upload (with an untracked-range guard, so a full range table no longer forces a re-upload on every bind), the page-fault hook built from scratch for OHOS (`OC_ES_ESR` is always 0 off Apple platforms, so "was this a write?" has to be decided another way), and **BC→ASTC re-encoding on the device** so a GPU without BC support still gets the terrain.
- **The socket layer went from "always ENETDOWN" to real BSD sockets**: the game's single-player mode starts two shards (Server Master / Secondary) inside its own process and connects to `127.0.0.1` with RakNet. Without real sockets it sits on "starting server" forever.
- **Asset path resolution**: three-tier fallback plus a lazy full basename index, **audited offline across all 21,310 files**; the on-device proof is that the unresolved-reference file is never created at all.
- **The forensics channel**: pixels go through files, never through the log (hilog silently drops long and burst lines), crash scenes go to a file, thread dumps, and the sampler prints the last SVC number per thread — which is how you tell "waiting" from "spinning inside a syscall".
- **A scene fingerprint that needs no screenshot**: the per-frame draw count is stable enough per screen to identify it — 2 = boot, 12–14 = splash and title, 40–44 = submenu, 106–164 = the world-config modals, 651 = the roster, 750+ = a live world. Every automation round now decides "did the tap do anything" from that number instead of from a pixel diff, which is what caught a driver that had been "navigating" for 34 presses while the game sat toggling one panel (`@draws` flipping 142 ↔ 149 the whole time).

## What is blocking

The latest repair fixes three buffer-contract errors: arena allocation rounding, full-range validation before reusing mutable buffers, and a fallback vertex binding outside the device limit. The signed update is installed; the observed startup/world-settings screens no longer show the earlier tearing, and the user reports normal rendering. More world scenes and long runs remain to be covered. **[The repair report](docs/OHOS-RENDERING-2026-10.md)** describes the final changes; **[BLOCKERS.md](docs/BLOCKERS.md)** preserves the AN-round investigation.

Closed this week, and it was the biggest one: **the flat green ground**. The Maleoon 920 accepts almost no BC format, so the terrain atlases — bound 266,425 times — had no path onto the GPU. The host now re-encodes BC1/BC3/BC4/BC5 into ASTC 4×4 itself, on the device at upload time through a cross-built astcenc 5.7.0, with an on-disk cache so a second boot reads back instead of re-encoding, and the same conversion offline for a full pre-bake. `ASTC缓存 命中/新写/盘错/额尽` reports which of the two happened.

Four further open accounts: the **root cause** of the lost wakeup is not fixed (a spurious-wake fallback masks it), the render pass ignores the attachment level/layer the guest names (always level 0, layer 0 — needs a per-subresource `VkImageView` cache), performance (23.7–30.0 fps submitted in-world by the `hidumper` counter, p99 ~250 ms, world generation takes 2m20s against 20–40 s originally — and that fps figure is a *submission* rate, so it is not yet a clean gameplay measurement), and a real text input box plus touch finger reporting.

## Running it yourself

1. **[docs/UNPACKING.md](docs/UNPACKING.md)** — from **a game copy you own** and **keys exported from your own console**, extract the three modules and the romfs, run the instruction-level patches, build the two romfs layers. These products are not in this repository.
2. **[docs/BUILD-OHOS.md](docs/BUILD-OHOS.md)** — cross-compile `libdmhost.so`, drop it into the HAP project, sign, install.
3. **[docs/RUN-TO-WORLD.md](docs/RUN-TO-WORLD.md)** — the verified path from an installed HAP to a live world: the launch parameters that are **required** (without `DM_RT_ASTC` the ground renders all black; launching from the home-screen icon turns every switch off), the two-step install for the 2.2 GB HAP, and the `@draws` scene fingerprints used to drive the menus.
4. **[docs/DM-SWITCHES.md](docs/DM-SWITCHES.md)** — the launch switches; every diagnostic knob is documented here so you do not have to read the code.
5. **[docs/CONTENTS.md](docs/CONTENTS.md)** — exactly what is in this repository, what is not, and why each remaining item has to be there.

## Disclaimer and licence

This repository contains only our own and third-party open-source **code and documentation**: **no** game program, art, audio, text, data tables, shader binaries, or keys. Every piece of game content must be produced by the user from a copy they own. Don't Starve Together is a work of Klei Entertainment; Nintendo Switch is a trademark of Nintendo. This project is affiliated with neither and endorses neither. Do not use it to infringe copyright.

Code is released under the MIT licence (see [LICENSE](LICENSE)); provenance and third-party notices are in [NOTICE](NOTICE).
