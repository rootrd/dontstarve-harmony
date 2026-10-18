# Don't Starve Together (Switch) running natively on HarmonyOS — an HLE port

**Don't Starve Together**, the Nintendo Switch build, executed **natively** on a Huawei HarmonyOS phone. Not an emulator shell, not a remake, not cloud streaming: the guest's ARM64 machine code runs inside our HLE (high-level emulation) host, and every platform capability it asks for — threads and semaphores, file system, sockets, gamepad, audio, and the **NVN graphics API** — is served by a native HarmonyOS implementation. NVN is replayed **call by call into Vulkan** and drawn on a Maleoon 920.

**Status (2026-10-18): the game is fully playable up to walking around in a world.** EULA → main menu → create world → world generation → character select → in world, day-1 HUD all correct. One thing is still wrong: **the ground texture renders flat green** (characters, grass tufts, lighting, HUD and text are all fine).

<p align="center">
  <img src="docs/screens/main_menu.jpg" width="49%" alt="The real main menu: Wilson, the menu column, the offline badge">
  <img src="docs/screens/in_world.jpg" width="49%" alt="In the world: day 1 dial, inventory bar, interaction prompt all render; the ground is flat green">
</p>
<p align="center"><sub>Left: the main menu renders complete. Right: actually in the world (day counter, inventory bar, an interaction prompt) — the flat green ground is the one remaining functional blocker.</sub></p>

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
- **NVN→Vulkan replay**: 18 entry points the guest wanted and the host lacked; viewport Y-flip (with the scissor box following) fixed the vertical mirror; **the real cause of flat-fill UI was a mis-indexed NVN→Vk blend factor table**. The shader library went 96 → 222 keys while "no library" fell from 41% to 10%, and the key scheme was then rebuilt so a library survives a re-pack: 40 entries now cover all 27 keys the device reports.
- **Texture upload chain**: packaged textures stage on their own 160 MB ring instead of fighting the vertex stream for the 64 MB arena, dirty-page re-upload (with an untracked-range guard, so a full range table no longer forces a re-upload on every bind), and the page-fault hook built from scratch for OHOS (`OC_ES_ESR` is always 0 off Apple platforms, so "was this a write?" has to be decided another way).
- **The socket layer went from "always ENETDOWN" to real BSD sockets**: the game's single-player mode starts two shards (Server Master / Secondary) inside its own process and connects to `127.0.0.1` with RakNet. Without real sockets it sits on "starting server" forever.
- **Asset path resolution**: three-tier fallback plus a lazy full basename index, **audited offline across all 21,310 files**; the on-device proof is that the unresolved-reference file is never created at all.
- **The forensics channel**: pixels go through files, never through the log (hilog silently drops long and burst lines), crash scenes go to a file, thread dumps, and the sampler prints the last SVC number per thread — which is how you tell "waiting" from "spinning inside a syscall".

## What is blocking

**The flat green ground** — the only functional blocker. Evidence: the terrain atlases (the 2048×2048 and 1024×512 ones) are bound **266,425 times** but never once enter the upload queue; the binding site reads `u0 d0 b0 t0 r0` (never uploaded, never drawn, never queued). Two hypotheses are already dead (mip clamped to level 0, missing asset files). The split is two probes: does the atlas ever reach `pkg_queue`, and does the shader even look at the slot where the handle sits (`set_fill`'s all-empty branch). Details, measurements and the next step: **[docs/BLOCKERS.md](docs/BLOCKERS.md)**.

Four further open accounts: the **root cause** of the lost wakeup is not fixed (a spurious-wake fallback masks it), the pack staging and the vertex stream share one arena, performance (~30 fps, p99 ~250 ms, world generation takes 2m20s against 20–40 s originally), and a real text input box plus touch finger reporting.

## Running it yourself

1. **[docs/UNPACKING.md](docs/UNPACKING.md)** — from **a game copy you own** and **keys exported from your own console**, extract the three modules and the romfs, run the instruction-level patches, build the two romfs layers. These products are not in this repository.
2. **[docs/BUILD-OHOS.md](docs/BUILD-OHOS.md)** — cross-compile `libdmhost.so`, drop it into the HAP project, sign, install.
3. **[docs/DM-SWITCHES.md](docs/DM-SWITCHES.md)** — the launch switches; every diagnostic knob is documented here so you do not have to read the code.
4. **[docs/CONTENTS.md](docs/CONTENTS.md)** — exactly what is in this repository, what is not, and why each remaining item has to be there.

## Disclaimer and licence

This repository contains only our own and third-party open-source **code and documentation**: **no** game program, art, audio, text, data tables, shader binaries, or keys. Every piece of game content must be produced by the user from a copy they own. Don't Starve Together is a work of Klei Entertainment; Nintendo Switch is a trademark of Nintendo. This project is affiliated with neither and endorses neither. Do not use it to infringe copyright.

Code is released under the MIT licence (see [LICENSE](LICENSE)); provenance and third-party notices are in [NOTICE](NOTICE).
