# What is in this repository, and what is not

The front page says the project "contains none and distributes none" of the game's content. This page makes that precise, because a blanket statement is not checkable.

## Not here

- **The game program.** Not one byte of `main`, `sdk` or `subsdk0`, nor any patched variant of them.
- **Art, audio, video, fonts, text tables, data tables** — i.e. the entire romfs (current 1.30.0 extraction: 21,306 files, 5,149,111,248 bytes across two layers).
- **Shaders** — neither the original packages nor anything translated from them, including the shader library this port built (40 entries on the current key chain; 222 and 233 on the superseded one).
- **Keys.** No `prod.keys`, no title keys, no console-specific material.
- **The instruction-level patch tables** for this title (`*.sites.tsv`, `*.fixups.tsv`, …). They are derived from the game binary, so they are game-derived data by the same rule; how to regenerate them is in [UNPACKING.md](UNPACKING.md).
- **Signing material** — certificates, profiles, keystores, and the `signingConfigs` block that references them.
- **Screenshots other than the two on the front page**, and no captured logs, frame dumps or crash files. Two of the candidate frames were rejected on exactly this ground: one showed a chat notification banner, one was a photo of the device's home screen.
- **The Diablo II branch's derived records.** This host layer came from [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile); when it was copied here, its game-derived inputs were left behind: `shader_map.tsv`, `shader_facts.tsv`, `pso_manifest.tsv`, `nvn_slots.tsv`, `dm/data/casc_overlay/`, and `dm/verify/` (whose fixtures were captured from a running game). The offline shader tools that consume some of these are still here, and will tell you plainly that the table is missing.

## Here, and why it has to be

These are records of **facts about** a console API or about our own code — not game content. Without them you cannot build.

| Where | Size | What it is | Why it must be here |
|---|---:|---|---|
| `dm/gfx/nvn/include/nvn.h` | 89 KB | Declarations of the console graphics API. Third-party work under MIT, author named in the file header | The replay layer implements against it |
| `dm/gfx/nvn/names.txt` | 16 KB | The function names of that API | The host takes over these functions **by name** ([hle_nvn.c](../dm/gfx/nvn/hle_nvn.c) reads it) |
| `dm/gfx/nvn/nvn_protos.h`, `nvn_obj.h`, `nvn_count.h`, `nvn_thunks.S`, `dm/ohos/nvn_thunks_ohos.S` | ~250 KB | Generated entry points and object tables for that API | Written by us from the public declaration set |
| `dm/gfx/nvn/nvn_sh_tri.h`, `shaders/tri.*` | ~15 KB | SPIR-V for **our own** full-screen present triangle | Generated with `glslang` from our own GLSL; nothing to do with the game |
| `dm/third_party/**` | 4.8 MB | `libdeflate`, `bcdec`, `flip`, `stb`, and a subset of Ryujinx's shader decoder | Third-party open source, each with its own licence file in place |
| `third_party/astcenc/{include/astcenc.h,LICENSE.txt,README.md}` | ~50 KB | the public header of ARM's astcenc 5.7.0 (Apache-2.0) and its licence text | the only part of astcenc published here. The cross-compiled `.a` files are build products, gitignored; [README](../third_party/astcenc/README.md) documents the toolchain so anyone can produce them |
| `dm/host/*.c`, `dm/gfx/**/*.c`, `dm/ohos/*` | ~5 MB | our code | the port |
| `scripts/**`, `dm/gfx/nvn/tests/*.c` | source only | build/pack helpers plus the host-side fault-injection test for the upload path | `scripts/pkg_faults.sh` extracts the production bodies and asserts them on a plain Linux host — it is how the "verified offline" numbers in [STATUS.md](STATUS.md) can be re-produced without a phone |
| `docs/screens/*.jpg` | ~1 MB | four screenshots of this port running: the main menu, a live world, and the same title screen before/after the vertex-and-dynamic-state round | they are the status. Everything else about the state is text |

The generated game shader library remains under ignored `build/`; `nvn_sh_tri.h` contains only our own present shaders. Current repair details and local verification commands are in [OHOS-RENDERING-2026-10.md](OHOS-RENDERING-2026-10.md). No new device screenshots or logs were added in this repair.

### About the inherited documentation and tools

The per-directory pages under `dm/` (`dm/README.md`, `dm/host/README.md`, `dm/gfx/README.md`, `dm/tools/README.md`, …) and a few scripts in `dm/tools/` (`extract_game.sh`, `d2rm_pack.py`, `casc_overlay.py`, `mac_app.sh`) came from the sibling branch and still describe that branch where they mention Xcode, Metal, iOS or Diablo II. They are kept because the shared host layer they document is the same code, and because deleting them would break the map of `host/` and `gfx/`. [dm/README.md](../dm/README.md) opens with a banner saying exactly which sentences belong to the other branch. `extract_game.sh` additionally pins three SHA-256 values and a title id — those are identifiers used to check that a dump is the expected version, not game content.

### About addresses in the source

Like every HLE project, our code refers to locations inside the guest binary: `sdk+0x1cbcf4`, `main+0x45f9d0`, and so on, plus the symbol names the host stubs out (`_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE`). Roughly three thousand such constants appear across the tree.

They are **not usable without the game**, and they are **not a copy of anything**: an offset is a fact about where something is, the same class of statement as "this import is named X". They are also version-specific — valid for build 1.30 only, and wrong for any other. Where an offset encodes *what the game does*, we describe the behaviour in prose instead of shipping the game's own bytes.

### About the two screenshots

They show a game running. They are here for the same reason a reviewer includes a figure: they are the evidence that the claim in the headline is true, and one of them is the evidence of the open bug. They are the only game-derived visual material in the repository, and they were chosen so that nothing else (notifications, launcher, home screen, chat) appears in frame.
