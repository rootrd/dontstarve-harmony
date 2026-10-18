# Getting the game files (BYOG)

**Nothing in this repository is game content.** You must produce everything below yourself, from a copy of the game you own, using keys exported from your own console. Do not ask in issues for links to game files, keys, or dumps, and do not post them here. This project is offline single-player only and is not affiliated with or endorsed by Klei Entertainment or Nintendo.

The pipeline has four stages. The host expects their output at fixed paths; those paths are the contract, and everything before them is your business.

```
your own cartridge / digital copy
   └─(1) extract + decrypt with your own keys ─→ 3 NSO modules + a romfs directory tree
                                                     └─(2) instruction-level patch ─→ *.patched.bin + patch tables
                                                                                         └─(3) assemble two romfs layers
                                                                                                 └─(4) put them in the HAP
```

## 1. Modules and romfs

The Switch build of Don't Starve Together used here presents as three code modules plus a data partition:

| Item | Where it comes from | What it is |
|---|---|---|
| `main` | the title's program partition (ExeFS) | the game's own code |
| `sdk` | same | Nintendo's SDK, statically linked by the game |
| `subsdk0` | same | an additional linked module (middleware) |
| `romfs` | the same partition, or the update layer | 21,310 data files across two layers |

Requirements, in words rather than commands, because the tooling changes and the licence of any given tool is its own:

- A standard NCA/NSP extraction tool, run with **your own `prod.keys`** (and, for a title you bought digitally, the ticket's title key). Filter the key file down to the keys the tool actually needs first; some versions of some tools abort on unknown key names.
- The NSO modules must be **decompressed** before use. Our loader takes plain, uncompressed NSO images.
- If the game has an update, the **update's** program partition is the one you want; a base-cartridge `main` will not match the patch tables and every offset-based assumption below becomes wrong.
- Expect these traps, all of which we hit: an XCI image may carry no ticket; an update NCA can be missing data sections; and newer-SDK romfs layouts make some extraction tools exit hard rather than warn.

**Version pinning matters more than anything else in this file.** Everything measured in this repository is against build **1.30**, title id `755637032`. A different build shifts every offset.

## 2. Instruction-level patches

The guest modules are not run as-is. Before loading, a small set of rewrites is applied, and the result is a `*.patched.bin` per module plus a sidecar table describing what was done. The categories, and why each exists:

| Table | What it records | Why the host needs it |
|---|---|---|
| `*.sites.tsv` | offset → rewrite class (`tpidr_r`, `x18`) | Horizon uses `TPIDR_EL0` and `X18` in ways the host must take over; ~23,800 sites across the three modules |
| `*.fixups.tsv` | layout facts: text/ro/data bounds, tail region, **BSS size**, patch region, key-slot indices | the guest's BSS is enormous (≈3.3 GB of reservation), so the mapping strategy is decided from these numbers, not guessed |
| `*.cuts.tsv` | function-entry cuts | replace a guest routine with a host implementation when a symbol-level stub is not enough |
| `*.gates.tsv` / `*.obs.tsv` / `*.params.tsv` | optional gates, observations, parameter overrides | diagnostics only; empty for this title |
| `build_src.sha256` | hash of the patcher itself | **self-audit**: proves the artefacts on disk came from the patcher in the tree |

**These tables are derived from the game binary and are therefore not published here**, exactly like the binary itself. To reproduce them you generate them from your own modules with the tooling in `dm/tools/` (`nso.py` to read the image, `patch_nso.py` for the rewrite pass, `hle_inventory.py` to list which imported symbols the host must provide). The starting point for `sites.tsv` is mechanical: disassemble, find every `MRS x?, TPIDR_EL0` and every `ADR/ADD/LDR` through `X18`, emit `offset<TAB>class`.

What the host does with them is in [BUILD-OHOS.md](BUILD-OHOS.md); what it *requires* of them is in `dm/host/loader.c`.

## 3. Two romfs layers

The data partition is split across two directories because of a HAP size limit, not because of anything the game does:

| Layer | Files | Size | Content |
|---|---:|---:|---|
| primary | 18,086 | 2.1 GB | the bulk of the romfs |
| overlay | 3,224 | 2.8 GB | the larger archives, mounted as a second search root |

The host mounts both and searches them in order, then falls back to a lazy full basename index (see [BLOCKERS.md](BLOCKERS.md#1-the-flat-green-ground-the-only-functional-blocker) — the path-resolution story). Three files in the tree are legitimately zero bytes; do not treat them as extraction failures.

**Do not put the romfs in the app's save directory** and do not expect to mount it from a downloaded bundle: during bring-up the decision was that all resources ship inside the install package, because a separate "transfer the data to the device" step turns every test round into two failure modes instead of one.

## 4. Into the HAP

The two layers become `resfile` content of two HAP modules (the resource directory the app can read directly at a stable path, rather than `rawfile`, which is compressed and not path-addressable). The mount points the host expects are documented in [BUILD-OHOS.md](BUILD-OHOS.md).

## 5. Verify before you debug anything

Before blaming the emulator, confirm the data side:

1. `build_src.sha256` matches the patcher you ran.
2. The patched module sizes and the `TEXT_END`/`RO_OFF`/`DATA_OFF` numbers in `fixups.tsv` agree with the image.
3. Launch once with the unresolved-path list switched on (`DM_MISSLIST=<path>`, see [DM-SWITCHES.md](DM-SWITCHES.md)). **The correct result is that the file is never created.** If it exists, every guest path that could not be resolved is listed there, and that is a data problem, not a graphics problem.
4. A wrong or stale artefact silently invalidates an entire device round — a bad package does not fail loudly, it just measures nothing. Check the product before installing it.
